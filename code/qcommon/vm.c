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
// vm.c -- virtual machine

/*


intermix code and data
symbol table

a dll has one imported function: VM_SystemCall
and one exported function: Perform


*/

#include "vm_local.h"


vm_t	*currentVM = NULL;
vm_t	*lastVM    = NULL;
int		vm_debugLevel;

// used by Com_Error to get rid of running vm's before longjmp
static int forced_unload;

static cvar_t	*vm_linked;

#define	MAX_VM		3
vm_t	vmTable[MAX_VM];


void VM_VmInfo_f( void );
void VM_VmProfile_f( void );



#if 0 // 64bit!
// converts a VM pointer to a C pointer and
// checks to make sure that the range is acceptable
void	*VM_VM2C( vmptr_t p, int length ) {
	return (void *)p;
}
#endif

void VM_Debug( int level ) {
	vm_debugLevel = level;
}

/*
==============
VM_Init
==============
*/
void VM_Init( void ) {
	Cvar_Get( "vm_cgame", "2", CVAR_ARCHIVE );	// !@# SHIP WITH SET TO 2
	Cvar_Get( "vm_game", "2", CVAR_ARCHIVE );	// !@# SHIP WITH SET TO 2
	Cvar_Get( "vm_ui", "2", CVAR_ARCHIVE );		// !@# SHIP WITH SET TO 2
	// native code, so only the player or the operator chooses it, never a
	// server or a game module
	vm_linked = Cvar_Get( "vm_linked", "1", CVAR_PROTECTED );
	Cvar_SetDescription( vm_linked, "Use the game modules linked into the executable when running the base game" );

	Cmd_AddCommand ("vmprofile", VM_VmProfile_f );
	Cmd_AddCommand ("vminfo", VM_VmInfo_f );

	Com_Memset( vmTable, 0, sizeof( vmTable ) );
}


/*
===============
VM_ValueToSymbol

Assumes a program counter value
===============
*/
const char *VM_ValueToSymbol( vm_t *vm, int value ) {
	vmSymbol_t	*sym;
	static char		text[MAX_TOKEN_CHARS];

	sym = vm->symbols;
	if ( !sym ) {
		return "NO SYMBOLS";
	}

	// find the symbol
	while ( sym->next && sym->next->symValue <= value ) {
		sym = sym->next;
	}

	if ( value == sym->symValue ) {
		return sym->symName;
	}

	Com_sprintf( text, sizeof( text ), "%s+%i", sym->symName, value - sym->symValue );

	return text;
}

/*
===============
VM_ValueToFunctionSymbol

For profiling, find the symbol behind this value
===============
*/
vmSymbol_t *VM_ValueToFunctionSymbol( vm_t *vm, int value ) {
	vmSymbol_t	*sym;
	static vmSymbol_t	nullSym;

	sym = vm->symbols;
	if ( !sym ) {
		return &nullSym;
	}

	while ( sym->next && sym->next->symValue <= value ) {
		sym = sym->next;
	}

	return sym;
}


/*
===============
VM_SymbolToValue
===============
*/
int VM_SymbolToValue( vm_t *vm, const char *symbol ) {
	vmSymbol_t	*sym;

	for ( sym = vm->symbols ; sym ; sym = sym->next ) {
		if ( !strcmp( symbol, sym->symName ) ) {
			return sym->symValue;
		}
	}
	return 0;
}


/*
=====================
VM_SymbolForCompiledPointer
=====================
*/
#if 0 // 64bit!
const char *VM_SymbolForCompiledPointer( vm_t *vm, void *code ) {
	int			i;

	if ( code < (void *)vm->codeBase ) {
		return "Before code block";
	}
	if ( code >= (void *)(vm->codeBase + vm->codeLength) ) {
		return "After code block";
	}

	// find which original instruction it is after
	for ( i = 0 ; i < vm->codeLength ; i++ ) {
		if ( (void *)vm->instructionPointers[i] > code ) {
			break;
		}
	}
	i--;

	// now look up the bytecode instruction pointer
	return VM_ValueToSymbol( vm, i );
}
#endif



/*
===============
ParseHex
===============
*/
int	ParseHex( const char *text ) {
	int		value;
	int		c;

	value = 0;
	while ( ( c = *text++ ) != 0 ) {
		if ( c >= '0' && c <= '9' ) {
			value = value * 16 + c - '0';
			continue;
		}
		if ( c >= 'a' && c <= 'f' ) {
			value = value * 16 + 10 + c - 'a';
			continue;
		}
		if ( c >= 'A' && c <= 'F' ) {
			value = value * 16 + 10 + c - 'A';
			continue;
		}
	}

	return value;
}

/*
===============
VM_LoadSymbols
===============
*/
void VM_LoadSymbols( vm_t *vm ) {
	union {
		char	*c;
		void	*v;
	} mapfile;
	char *text_p, *token;
	char	name[MAX_QPATH];
	char	symbols[MAX_QPATH];
	vmSymbol_t	**prev, *sym;
	int		count;
	int		value;
	int		chars;
	int		segment;
	int		numInstructions;

	// don't load symbols if not developer
	if ( !com_developer->integer ) {
		return;
	}

	COM_StripExtension(vm->name, name, sizeof(name));
	Com_sprintf( symbols, sizeof( symbols ), "vm/%s.map", name );
	FS_ReadFile( symbols, &mapfile.v );
	if ( !mapfile.c ) {
		Com_Printf( "Couldn't load symbol file: %s\n", symbols );
		return;
	}

	numInstructions = vm->instructionCount;

	// parse the symbols
	text_p = mapfile.c;
	prev = &vm->symbols;
	count = 0;

	while ( 1 ) {
		token = COM_Parse( &text_p );
		if ( !token[0] ) {
			break;
		}
		segment = ParseHex( token );
		if ( segment ) {
			COM_Parse( &text_p );
			COM_Parse( &text_p );
			continue;		// only load code segment values
		}

		token = COM_Parse( &text_p );
		if ( !token[0] ) {
			Com_Printf( "WARNING: incomplete line at end of file\n" );
			break;
		}
		value = ParseHex( token );

		token = COM_Parse( &text_p );
		if ( !token[0] ) {
			Com_Printf( "WARNING: incomplete line at end of file\n" );
			break;
		}
		chars = strlen( token );
		sym = Hunk_Alloc( sizeof( *sym ) + chars, h_high );
		*prev = sym;
		prev = &sym->next;
		sym->next = NULL;

		// convert value from an instruction number to a code offset
		if ( value >= 0 && value < numInstructions ) {
			value = vm->instructionPointers[value];
		}

		sym->symValue = value;
		Q_strncpyz( sym->symName, token, chars + 1 );

		count++;
	}

	vm->numSymbols = count;
	Com_Printf( "%i symbols parsed from %s\n", count, symbols );
	FS_FreeFile( mapfile.v );
}

/*
============
VM_DllSyscall

Dlls will call this directly

 rcg010206 The horror; the horror.

  The syscall mechanism relies on stack manipulation to get its args.
   This is likely due to C's inability to pass "..." parameters to
   a function in one clean chunk. On PowerPC Linux, these parameters
   are not necessarily passed on the stack, so while (&arg[0] == arg)
   is true, (&arg[1] == 2nd function parameter) is not necessarily
   accurate, as arg's value might have been stored to the stack or
   other piece of scratch memory to give it a valid address, but the
   next parameter might still be sitting in a register.

  Quake's syscall system also assumes that the stack grows downward,
   and that any needed types can be squeezed, safely, into a signed int.

  This hack below copies all needed values for an argument to a
   array in memory, so that Quake can get the correct values. This can
   also be used on systems where the stack grows upwards, as the
   presumably standard and safe stdargs.h macros are used.

  As for having enough space in a signed int for your datatypes, well,
   it might be better to wait for DOOM 3 before you start porting.  :)

  The original code, while probably still inherently dangerous, seems
   to work well enough for the platforms it already works on. Rather
   than add the performance hit for those platforms, the original code
   is still in use there.

  For speed, we just grab 15 arguments, and don't worry about exactly
   how many the syscall actually needs; the extra is thrown away.
 
============
*/
intptr_t QDECL VM_DllSyscall( intptr_t arg, ... ) {
#if !id386 || defined __clang__
  // rcg010206 - see commentary above
  intptr_t args[MAX_VMSYSCALL_ARGS];
  int i;
  va_list ap;
  
  args[0] = arg;
  
  va_start(ap, arg);
  for (i = 1; i < ARRAY_LEN (args); i++)
    args[i] = va_arg(ap, intptr_t);
  va_end(ap);
  
  return currentVM->systemCall( args );
#else // original id code
	return currentVM->systemCall( &arg );
#endif
}


/*
=================
VM_ValidateHeader

Byte swaps the header of a .qvm file and checks it against the file's
length. Returns what's wrong with it, or NULL.
=================
*/
static const char *VM_ValidateHeader( vmHeader_t *header, long fileLength )
{
	long	headerLength;
	int		magic;
	int		jtrgLength;
	int		i;

	if ( fileLength < (long)sizeof( int ) ) {
		return "truncated header";
	}

	magic = LittleLong( header->vmMagic );
	if ( magic == VM_MAGIC_VER2 ) {
		headerLength = sizeof( vmHeader_t );
	} else if ( magic == VM_MAGIC ) {
		// the 1.32b header ends before jtrgLength
		headerLength = sizeof( vmHeader_t ) - sizeof( int );
	} else {
		return "no recognisable magic number in its header";
	}

	if ( fileLength < headerLength ) {
		return "truncated header";
	}

	// byte swap the header
	for ( i = 0 ; i < headerLength / 4 ; i++ ) {
		((int *)header)[i] = LittleLong( ((int *)header)[i] );
	}

	jtrgLength = magic == VM_MAGIC_VER2 ? header->jtrgLength : 0;

	if ( jtrgLength < 0
		|| header->bssLength < 0
		|| header->dataLength < 0
		|| header->litLength < 0
		|| header->codeLength <= 0 ) {
		return "negative segment length";
	}

	// each instruction takes at least a byte
	if ( header->instructionCount <= 0 || header->instructionCount > header->codeLength ) {
		return "bad instruction count";
	}

	// the segments must lie inside the file; the sums are in 64 bits so
	// they can't overflow
	if ( header->codeOffset < headerLength
		|| (int64_t)header->codeOffset + header->codeLength > fileLength ) {
		return "code segment outside the file";
	}

	if ( header->dataOffset < headerLength
		|| (int64_t)header->dataOffset + header->dataLength + header->litLength + jtrgLength > fileLength ) {
		return "data segment outside the file";
	}

	if ( header->codeLength > VM_MAX_CODE_LENGTH
		|| (int64_t)header->dataLength + header->litLength + header->bssLength > VM_MAX_DATA_LENGTH ) {
		return "segment too large";
	}

	return NULL;
}

/*
=================
VM_OperandLength

The length in bytes of an opcode's operand in a .qvm file
=================
*/
int VM_OperandLength( int op )
{
	switch ( op ) {
	case OP_ENTER:
	case OP_CONST:
	case OP_LOCAL:
	case OP_LEAVE:
	case OP_EQ:
	case OP_NE:
	case OP_LTI:
	case OP_LEI:
	case OP_GTI:
	case OP_GEI:
	case OP_LTU:
	case OP_LEU:
	case OP_GTU:
	case OP_GEU:
	case OP_EQF:
	case OP_NEF:
	case OP_LTF:
	case OP_LEF:
	case OP_GTF:
	case OP_GEF:
	case OP_BLOCK_COPY:
		return 4;
	case OP_ARG:
		return 1;
	default:
		return 0;
	}
}

/*
=================
VM_ValidateCode

Checks, for every backend, that each instruction is one the VM defines
and lies inside the code, and that the code ends with OP_LEAVE or
OP_JUMP, as q3asm's always does, so that no backend runs on past it.
Returns what's wrong with the code, or NULL.
=================
*/
static const char *VM_ValidateCode( const vmHeader_t *header )
{
	const byte	*code = (const byte *)header + header->codeOffset;
	int			pc = 0;
	int			op = OP_UNDEF;
	int			i;

	for ( i = 0 ; i < header->instructionCount ; i++ ) {
		if ( pc >= header->codeLength ) {
			return "fewer instructions than its header says";
		}

		op = code[pc];
		if ( op >= OP_MAX ) {
			return "undefined opcode";
		}

		pc += 1 + VM_OperandLength( op );
		if ( pc > header->codeLength ) {
			return "operand past the end of the code";
		}
	}

	if ( op != OP_LEAVE && op != OP_JUMP ) {
		return "doesn't end with OP_LEAVE or OP_JUMP";
	}

	return NULL;
}

/*
=================
VM_LoadQVM

Load a .qvm file
=================
*/
vmHeader_t *VM_LoadQVM( vm_t *vm, qboolean alloc, qboolean unpure)
{
	int					dataLength;
	int					i;
	long				fileLength;
	const char			*error;
	char				filename[MAX_QPATH];
	union {
		vmHeader_t	*h;
		void				*v;
	} header;

	// load the image
	Com_sprintf( filename, sizeof(filename), "vm/%s.qvm", vm->name );
	Com_Printf( "Loading vm file %s...\n", filename );

	fileLength = FS_ReadFileDir(filename, vm->searchPath, unpure, &header.v);

	if ( !header.h ) {
		Com_Printf( "Failed.\n" );
		VM_Free( vm );

		Com_Printf(S_COLOR_YELLOW "Warning: Couldn't open VM file %s\n", filename);

		return NULL;
	}

	// show where the qvm was loaded from
	FS_Which(filename, vm->searchPath);

	error = VM_ValidateHeader( header.h, fileLength );
	if ( error ) {
		VM_Free( vm );
		FS_FreeFile( header.v );

		Com_Printf( S_COLOR_YELLOW "Warning: %s has a bad header: %s\n", filename, error );
		return NULL;
	}

	error = VM_ValidateCode( header.h );
	if ( error ) {
		VM_Free( vm );
		FS_FreeFile( header.v );

		Com_Printf( S_COLOR_YELLOW "Warning: %s has bad code: %s\n", filename, error );
		return NULL;
	}

	if ( header.h->vmMagic == VM_MAGIC_VER2 ) {
		Com_Printf( "...which has vmMagic VM_MAGIC_VER2\n" );
	}

	// round up to next power of 2 so all data operations can
	// be mask protected
	dataLength = header.h->dataLength + header.h->litLength +
		header.h->bssLength;
	// q3asm puts the stack at the end of bss, but a smaller file would
	// put the stack below the start of the data
	if ( dataLength < PROGRAM_STACK_SIZE ) {
		dataLength = PROGRAM_STACK_SIZE;
	}
	for ( i = 0 ; dataLength > ( 1 << i ) ; i++ ) {
	}
	dataLength = 1 << i;

	if(alloc)
	{
		// allocate zero filled space for initialized and uninitialized data
		// leave some space beyond data mask so we can secure all mask operations
		vm->dataAlloc = dataLength + 4;
		vm->dataBase = Hunk_Alloc(vm->dataAlloc, h_high);
		vm->dataMask = dataLength - 1;
	}
	else
	{
		// clear the data, but make sure we're not clearing more than allocated
		if(vm->dataAlloc != dataLength + 4)
		{
			VM_Free(vm);
			FS_FreeFile(header.v);

			Com_Printf(S_COLOR_YELLOW "Warning: Data region size of %s not matching after "
					"VM_Restart()\n", filename);
			return NULL;
		}
		
		Com_Memset(vm->dataBase, 0, vm->dataAlloc);
	}

	// copy the intialized data
	Com_Memcpy( vm->dataBase, (byte *)header.h + header.h->dataOffset,
		header.h->dataLength + header.h->litLength );

	// byte swap the longs
	for ( i = 0 ; i < header.h->dataLength ; i += 4 ) {
		*(int *)(vm->dataBase + i) = LittleLong( *(int *)(vm->dataBase + i ) );
	}

	if(header.h->vmMagic == VM_MAGIC_VER2)
	{
		int previousNumJumpTableTargets = vm->numJumpTableTargets;

		header.h->jtrgLength &= ~0x03;

		vm->numJumpTableTargets = header.h->jtrgLength >> 2;
		Com_Printf("Loading %d jump table targets\n", vm->numJumpTableTargets);

		if(alloc)
		{
			vm->jumpTableTargets = Hunk_Alloc(header.h->jtrgLength, h_high);
		}
		else
		{
			if(vm->numJumpTableTargets != previousNumJumpTableTargets)
			{
				VM_Free(vm);
				FS_FreeFile(header.v);

				Com_Printf(S_COLOR_YELLOW "Warning: Jump table size of %s not matching after "
						"VM_Restart()\n", filename);
				return NULL;
			}

			Com_Memset(vm->jumpTableTargets, 0, header.h->jtrgLength);
		}

		Com_Memcpy(vm->jumpTableTargets, (byte *) header.h + header.h->dataOffset +
				header.h->dataLength + header.h->litLength, header.h->jtrgLength);

		// byte swap the longs
		for ( i = 0 ; i < header.h->jtrgLength ; i += 4 ) {
			*(int *)(vm->jumpTableTargets + i) = LittleLong( *(int *)(vm->jumpTableTargets + i ) );
		}
	}

	return header.h;
}

/*
=================
VM_Restart

Reload the data, but leave everything else in place
This allows a server to do a map_restart without changing memory allocation

We need to make sure that servers can access unpure QVMs (not contained in any pak)
even if the client is pure, so take "unpure" as argument.
=================
*/
vm_t *VM_Restart(vm_t *vm, qboolean unpure)
{
	vmHeader_t	*header;

	// native code can't be restarted in place
	if ( vm->entryPoint ) {
		char	name[MAX_QPATH];
		intptr_t	(*systemCall)( intptr_t *parms );
		
		systemCall = vm->systemCall;	
		Q_strncpyz( name, vm->name, sizeof( name ) );

		VM_Free( vm );

		vm = VM_Create( name, systemCall, VMI_NATIVE );
		return vm;
	}

	// load the image
	Com_Printf("VM_Restart()\n");

	if(!(header = VM_LoadQVM(vm, qfalse, unpure)))
	{
		Com_Error(ERR_DROP, "VM_Restart failed");
		return NULL;
	}

	// free the original file
	FS_FreeFile(header);

	return vm;
}

/*
================
VM_FindLinked

Game modules linked into the executable are the base game's code, so a mod
still loads its own from its game directory.
================
*/
static vmLinkedModule_t *VM_FindLinked( const char *module ) {
	vmLinkedModule_t	*linked;

	if ( !vm_linked->integer || Q_stricmp( FS_GetCurrentGameDir(), com_basegame->string ) ) {
		return NULL;
	}

	for ( linked = vm_linkedModules; linked->name; linked++ ) {
		if ( !Q_stricmp( linked->name, module ) ) {
			return linked;
		}
	}

	return NULL;
}

// MSVC defines __SANITIZE_ADDRESS__ too, but has no __attribute__, and
// can't build linked modules
#if defined( __SANITIZE_ADDRESS__ ) && defined( __GNUC__ )
#define VM_ADDRESS_SANITIZER
#elif defined( __has_feature )
#if __has_feature( address_sanitizer )
#define VM_ADDRESS_SANITIZER
#endif
#endif

/*
================
VM_CopyLinkedData

Copies a linked module's data, or zeroes it when from is NULL. The data
spans many objects, and AddressSanitizer puts redzones between them, which
it would report the copy for crossing; they hold nothing the copy harms.
================
*/
#ifdef VM_ADDRESS_SANITIZER
__attribute__(( no_sanitize_address ))
static void VM_CopyLinkedData( byte *to, const byte *from, size_t size ) {
	volatile byte	*out = to;

	while ( size-- ) {
		*out++ = from ? *from++ : 0;
	}
}
#else
static void VM_CopyLinkedData( byte *to, const byte *from, size_t size ) {
	if ( from ) {
		Com_Memcpy( to, from, size );
	} else {
		Com_Memset( to, 0, size );
	}
}
#endif

/*
================
VM_ResetLinked

Gives a linked module the data it had before its first load. Game code
expects a fresh copy each time it's loaded, as a dll or .qvm gets.
================
*/
static void VM_ResetLinked( vmLinkedModule_t *linked ) {
	size_t	dataSize = linked->dataEnd - linked->data;

	// the build brackets the module's objects with these markers; out of
	// order, the linker didn't keep the module's data together
	if ( linked->dataEnd <= linked->data || linked->bssEnd <= linked->bss ) {
		Com_Error( ERR_FATAL, "VM_Create: the linked %s's data isn't in one piece", linked->name );
	}

	if ( !linked->initialData ) {
		linked->initialData = Z_Malloc( dataSize );
		VM_CopyLinkedData( linked->initialData, linked->data, dataSize );
		return;
	}

	VM_CopyLinkedData( linked->data, linked->initialData, dataSize );
	VM_CopyLinkedData( linked->bss, NULL, linked->bssEnd - linked->bss );
}

/*
================
VM_Create

Uses the module linked into the executable if there is one,
otherwise searches the filesystem for a system dll or a .qvm
================
*/
vm_t *VM_Create( const char *module, intptr_t (*systemCalls)(intptr_t *), 
				vmInterpret_t interpret ) {
	vm_t		*vm;
	vmHeader_t	*header = NULL;
	vmLinkedModule_t	*linked;
	int			i, remaining, retval;
	char filename[MAX_OSPATH];
	void *startSearch = NULL;

	if ( !module || !module[0] || !systemCalls ) {
		Com_Error( ERR_FATAL, "VM_Create: bad parms" );
	}

	remaining = Hunk_MemoryRemaining();

	// see if we already have the VM
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		if (!Q_stricmp(vmTable[i].name, module)) {
			vm = &vmTable[i];
			return vm;
		}
	}

	// find a free vm
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		if ( !vmTable[i].name[0] ) {
			break;
		}
	}

	if ( i == MAX_VM ) {
		Com_Error( ERR_FATAL, "VM_Create: no free vm_t" );
	}

	vm = &vmTable[i];

	Q_strncpyz(vm->name, module, sizeof(vm->name));

	linked = VM_FindLinked( module );
	if ( linked ) {
		fileHandle_t	f;

		Com_Printf( "Using linked %s\n", module );

		// A pure server checks the paks the client's cgame and ui came
		// from, which the filesystem notes as it opens their QVMs. Open
		// the QVM this module stands in for, so its pak is noted too.
		FS_FOpenFileRead( va( "vm/%s.qvm", module ), &f, qfalse );
		if ( f ) {
			FS_FCloseFile( f );
		}

		VM_ResetLinked( linked );
		vm->entryPoint = linked->vmMain;
		vm->systemCall = systemCalls;
		linked->dllEntry( VM_DllSyscall );
		return vm;
	}

	do
	{
		retval = FS_FindVM(&startSearch, filename, sizeof(filename), module, (interpret == VMI_NATIVE));
		
		if(retval == VMI_NATIVE)
		{
			Com_Printf("Try loading dll file %s\n", filename);

			vm->dllHandle = Sys_LoadGameDll(filename, &vm->entryPoint, VM_DllSyscall);
			
			if(vm->dllHandle)
			{
				vm->systemCall = systemCalls;
				return vm;
			}
			
			Com_Printf("Failed loading dll, trying next\n");
		}
		else if(retval == VMI_COMPILED)
		{
			vm->searchPath = startSearch;
			if((header = VM_LoadQVM(vm, qtrue, qfalse)))
				break;

			// VM_Free overwrites the name on failed load
			Q_strncpyz(vm->name, module, sizeof(vm->name));
		}
	} while(retval >= 0);
	
	if(retval < 0)
		return NULL;

	vm->systemCall = systemCalls;

	// allocate space for the jump targets, which will be filled in by the compile/prep functions
	vm->instructionCount = header->instructionCount;
	vm->instructionPointers = Hunk_Alloc(vm->instructionCount * sizeof(*vm->instructionPointers), h_high);

	// copy or compile the instructions
	vm->codeLength = header->codeLength;

	vm->compiled = qfalse;

#ifndef HAVE_VM_COMPILED
	if(interpret >= VMI_COMPILED) {
		Com_Printf("Architecture doesn't have a bytecode compiler, using interpreter\n");
		interpret = VMI_BYTECODE;
	}
#else
	if(interpret != VMI_BYTECODE)
	{
		vm->compiled = qtrue;
		VM_Compile( vm, header );
	}
#endif
	// VM_Compile may have reset vm->compiled if compilation failed
	if (!vm->compiled)
	{
		VM_PrepareInterpreter( vm, header );
	}

	// free the original file
	FS_FreeFile( header );

	// load the map file
	VM_LoadSymbols( vm );

	// the stack is implicitly at the end of the image
	vm->programStack = vm->dataMask + 1;
	vm->stackBottom = vm->programStack - PROGRAM_STACK_SIZE;

	Com_Printf("%s loaded in %d bytes on the hunk\n", module, remaining - Hunk_MemoryRemaining());

	return vm;
}

/*
==============
VM_Free
==============
*/
void VM_Free( vm_t *vm ) {

	if(!vm) {
		return;
	}

	if(vm->callLevel) {
		if(!forced_unload) {
			Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
			return;
		} else {
			Com_Printf( "forcefully unloading %s vm\n", vm->name );
		}
	}

	if(vm->destroy)
		vm->destroy(vm);

	if ( vm->dllHandle ) {
		Sys_UnloadDll( vm->dllHandle );
		Com_Memset( vm, 0, sizeof( *vm ) );
	}
#if 0	// now automatically freed by hunk
	if ( vm->codeBase ) {
		Z_Free( vm->codeBase );
	}
	if ( vm->dataBase ) {
		Z_Free( vm->dataBase );
	}
	if ( vm->instructionPointers ) {
		Z_Free( vm->instructionPointers );
	}
#endif
	Com_Memset( vm, 0, sizeof( *vm ) );

	currentVM = NULL;
	lastVM = NULL;
}

void VM_Clear(void) {
	int i;
	for (i=0;i<MAX_VM; i++) {
		VM_Free(&vmTable[i]);
	}
}

void VM_Forced_Unload_Start(void) {
	forced_unload = 1;
}

void VM_Forced_Unload_Done(void) {
	forced_unload = 0;
}

void *VM_ArgPtr( intptr_t intValue ) {
	if ( !intValue ) {
		return NULL;
	}
	// currentVM is missing on reconnect
	if ( currentVM==NULL )
	  return NULL;

	if ( currentVM->entryPoint ) {
		return (void *)(currentVM->dataBase + intValue);
	}
	else {
		return (void *)(currentVM->dataBase + (intValue & currentVM->dataMask));
	}
}

/*
============
VM_GetValue

Answers trap_GetValue( char *value, int valueSize, const char *key ) for the
current module from its extensions, a table that ends with a NULL key. A
QVM's buffer must lie inside its memory; native and linked modules have no
bounds to check it against. A missing or empty buffer drops the module.
============
*/
intptr_t VM_GetValue( intptr_t *args, const vmExtension_t *extensions ) {
	intptr_t	value = args[1];
	intptr_t	valueSize = args[2];
	const char	*key = VMA(3);

	if ( !value || valueSize < 1 ) {
		Com_Error( ERR_DROP, "%s: trap_GetValue was given no buffer", currentVM->name );
	}
	if ( !currentVM->entryPoint && ( value < 0 || valueSize > (intptr_t)currentVM->dataMask + 1 ||
		value > (intptr_t)currentVM->dataMask + 1 - valueSize ) ) {
		Com_Error( ERR_DROP, "%s: trap_GetValue's buffer lies outside the module's memory", currentVM->name );
	}

	for ( ; extensions->key; extensions++ ) {
		if ( !Q_stricmp( key, extensions->key ) ) {
			Com_sprintf( (char *)(currentVM->dataBase + value), valueSize, "%i", extensions->trap );
			return qtrue;
		}
	}
	return qfalse;
}

void *VM_ExplicitArgPtr( vm_t *vm, intptr_t intValue ) {
	if ( !intValue ) {
		return NULL;
	}

	// currentVM is missing on reconnect here as well?
	if ( currentVM==NULL )
	  return NULL;

	//
	if ( vm->entryPoint ) {
		return (void *)(vm->dataBase + intValue);
	}
	else {
		return (void *)(vm->dataBase + (intValue & vm->dataMask));
	}
}


/*
==============
VM_Call


Upon a system call, the stack will look like:

sp+32	parm1
sp+28	parm0
sp+24	return value
sp+20	return address
sp+16	local1
sp+14	local0
sp+12	arg1
sp+8	arg0
sp+4	return stack
sp		return address

An interpreted function will immediately execute
an OP_ENTER instruction, which will subtract space for
locals from sp
==============
*/

intptr_t QDECL VM_Call( vm_t *vm, int callnum, ... )
{
	vm_t	*oldVM;
	intptr_t r;
	int i;

	if(!vm || !vm->name[0])
		Com_Error(ERR_FATAL, "VM_Call with NULL vm");

	oldVM = currentVM;
	currentVM = vm;
	lastVM = vm;

	if ( vm_debugLevel ) {
	  Com_Printf( "VM_Call( %d )\n", callnum );
	}

	++vm->callLevel;
	// if we have a dll loaded, call it directly
	if ( vm->entryPoint ) {
		//rcg010207 -  see dissertation at top of VM_DllSyscall() in this file.
		int args[MAX_VMMAIN_ARGS-1];
		va_list ap;
		va_start(ap, callnum);
		for (i = 0; i < ARRAY_LEN(args); i++) {
			args[i] = va_arg(ap, int);
		}
		va_end(ap);

		r = vm->entryPoint( callnum,  args[0],  args[1],  args[2], args[3],
                            args[4],  args[5],  args[6], args[7],
                            args[8],  args[9], args[10], args[11]);
	} else {
#if ( id386 || idsparc ) && !defined __clang__ // calling convention doesn't need conversion in some cases
#ifdef HAVE_VM_COMPILED
		if ( vm->compiled )
			r = VM_CallCompiled( vm, (int*)&callnum );
		else
#endif
			r = VM_CallInterpreted( vm, (int*)&callnum );
#else
		struct {
			int callnum;
			int args[MAX_VMMAIN_ARGS-1];
		} a;
		va_list ap;

		a.callnum = callnum;
		va_start(ap, callnum);
		for (i = 0; i < ARRAY_LEN(a.args); i++) {
			a.args[i] = va_arg(ap, int);
		}
		va_end(ap);
#ifdef HAVE_VM_COMPILED
		if ( vm->compiled )
			r = VM_CallCompiled( vm, &a.callnum );
		else
#endif
			r = VM_CallInterpreted( vm, &a.callnum );
#endif
	}
	--vm->callLevel;

	if ( oldVM != NULL )
	  currentVM = oldVM;
	return r;
}

//=================================================================

static int QDECL VM_ProfileSort( const void *a, const void *b ) {
	vmSymbol_t	*sa, *sb;

	sa = *(vmSymbol_t **)a;
	sb = *(vmSymbol_t **)b;

	if ( sa->profileCount < sb->profileCount ) {
		return -1;
	}
	if ( sa->profileCount > sb->profileCount ) {
		return 1;
	}
	return 0;
}

/*
==============
VM_VmProfile_f

==============
*/
void VM_VmProfile_f( void ) {
	vm_t		*vm;
	vmSymbol_t	**sorted, *sym;
	int			i;
	double		total;

	if ( !lastVM ) {
		return;
	}

	vm = lastVM;

	if ( !vm->numSymbols ) {
		return;
	}

	sorted = Z_Malloc( vm->numSymbols * sizeof( *sorted ) );
	sorted[0] = vm->symbols;
	total = sorted[0]->profileCount;
	for ( i = 1 ; i < vm->numSymbols ; i++ ) {
		sorted[i] = sorted[i-1]->next;
		total += sorted[i]->profileCount;
	}

	qsort( sorted, vm->numSymbols, sizeof( *sorted ), VM_ProfileSort );

	for ( i = 0 ; i < vm->numSymbols ; i++ ) {
		int		perc;

		sym = sorted[i];

		perc = 100 * (float) sym->profileCount / total;
		Com_Printf( "%2i%% %9i %s\n", perc, sym->profileCount, sym->symName );
		sym->profileCount = 0;
	}

	Com_Printf("    %9.0f total\n", total );

	Z_Free( sorted );
}

/*
==============
VM_VmInfo_f

==============
*/
void VM_VmInfo_f( void ) {
	vm_t	*vm;
	int		i;

	Com_Printf( "Registered virtual machines:\n" );
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		vm = &vmTable[i];
		if ( !vm->name[0] ) {
			break;
		}
		Com_Printf( "%s : ", vm->name );
		if ( vm->entryPoint ) {
			Com_Printf( vm->dllHandle ? "native\n" : "linked\n" );
			continue;
		}
		if ( vm->compiled ) {
			Com_Printf( "compiled on load\n" );
		} else {
			Com_Printf( "interpreted\n" );
		}
		Com_Printf( "    code length : %7i\n", vm->codeLength );
		Com_Printf( "    table length: %7i\n", vm->instructionCount*4 );
		Com_Printf( "    data length : %7i\n", vm->dataMask + 1 );
	}
}

/*
===============
VM_LogSyscalls

Insert calls to this while debugging the vm compiler
===============
*/
void VM_LogSyscalls( int *args ) {
	static	int		callnum;
	static	FILE	*f;

	if ( !f ) {
		f = fopen("syscalls.log", "w" );
	}
	callnum++;
	fprintf(f, "%i: %p (%i) = %i %i %i %i\n", callnum, (void*)(args - (int *)currentVM->dataBase),
		args[0], args[1], args[2], args[3], args[4] );
}

/*
=================
VM_BlockCopy
Executes a block copy operation within currentVM data space
=================
*/

void VM_BlockCopy(unsigned int dest, unsigned int src, size_t n)
{
	unsigned int dataMask = currentVM->dataMask;

	if ((dest & dataMask) != dest
	|| (src & dataMask) != src
	|| ((dest + n) & dataMask) != dest + n
	|| ((src + n) & dataMask) != src + n)
	{
		Com_Error(ERR_DROP, "OP_BLOCK_COPY out of range!");
	}

	Com_Memcpy(currentVM->dataBase + dest, currentVM->dataBase + src, n);
}
