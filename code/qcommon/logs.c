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
// logs.c -- logs kept within a budget, so a server left alone for years
// never fills its disk. A log (qconsole.log, and the game's g_log,
// games.log by default; FS_IsLog) starts a new file when
// it's opened on a day after the one it started on, or past a size; the
// finished file is named by the day it started (games-2026-10-11.log, then
// .2), compressed with gzip on a thread of its own, and the oldest of them
// deleted to keep all of them within com_logSpace and com_logFreeSpace of
// the disk free, and those last written more than com_logDays days ago. In a
// browser, the home's logs keep within com_logSpace uncompressed, with no
// disk to measure for com_logFreeSpace

#include "q_shared.h"
#include "qcommon.h"

#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#ifdef USE_INTERNAL_ZLIB
#include "zlib.h"
#else
#include <zlib.h>
#endif

// a log starts a new file past this, or a quarter of com_logSpace if less
#define LOG_SPLIT_BYTES		( 64 << 20 )
// a log whose write failed is opened again only with this much free
#define LOG_ROOM_BYTES		( 1 << 20 )
#define MAX_LOG_STARTS		16
#define MAX_LOG_DIRS		8
#define MAX_LOG_JOBS		32

static cvar_t	*com_logSpace;
static cvar_t	*com_logFreeSpace;
static cvar_t	*com_logDays;

// the next local midnight after Log_Prune last ran, when it runs again
static time_t	logNextDay;

// the day each log opened in this run started, as yyyymmdd
typedef struct {
	char	path[MAX_OSPATH];
	int		day;
} logStart_t;

static logStart_t	logStarts[MAX_LOG_STARTS];
static int			nextLogStart;

// the directories logs are in, whose old logs share the budget
static char	logDirs[MAX_LOG_DIRS][MAX_OSPATH];
static int	numLogDirs;

// a finished log to compress, to path.gz; the thread touches nothing else
typedef struct {
	char		path[MAX_OSPATH];
	int			error;			// errno, or -1 for zlib's, if it failed
	long long	inBytes, outBytes;
	int			startTime;
} logJob_t;

static logJob_t		logJobs[MAX_LOG_JOBS];
static int			logJobHead, numLogJobs;
static sysThread_t	*logThread;	// compressing logJobs[logJobHead]

// a map is loading, or the program starting, when a log refused a rename
// may be copied instead (Log_Prepare); until the frame's end
static qboolean	logMapLoading = qtrue;

// no log is written while the disk is under com_logFreeSpace with no old
// logs left to delete, since this time
static qboolean	logPaused;
static char		logPausedAt[32];

/*
=============
Log_Day

A time's local date as yyyymmdd
=============
*/
static int Log_Day( time_t t ) {
	struct tm	*tm = localtime( &t );

	if ( !tm ) {
		return 0;
	}
	return ( tm->tm_year + 1900 ) * 10000 + ( tm->tm_mon + 1 ) * 100 + tm->tm_mday;
}

/*
=============
Log_DaysFrom

The local time the given number of days from today's start, -1 if it
can't be known
=============
*/
static time_t Log_DaysFrom( int days ) {
	time_t		now = time( NULL );
	struct tm	*tm = localtime( &now ), t;

	if ( !tm ) {
		return (time_t)-1;
	}
	// mktime normalizes the day, and the hour it finds for midnight on a
	// change to or from daylight saving
	t = *tm;
	t.tm_mday += days;
	t.tm_hour = t.tm_min = t.tm_sec = 0;
	t.tm_isdst = -1;
	return mktime( &t );
}

/*
=============
Log_SplitBytes

The size past which a log starts a new file
=============
*/
long long Log_SplitBytes( void ) {
	long long	quarter = (long long)com_logSpace->integer * ( 1 << 20 ) / 4;

	if ( quarter > 0 && quarter < LOG_SPLIT_BYTES ) {
		return quarter;
	}
	return LOG_SPLIT_BYTES;
}

/*
=============
Log_Free

The bytes free on the logs' disk, and the reserve com_logFreeSpace keeps;
qfalse if they're unknown, as in a browser
=============
*/
static qboolean Log_Free( unsigned long long *available, unsigned long long *reserve ) {
	unsigned long long	total;

	if ( !numLogDirs || !Sys_DiskSpace( logDirs[0], available, &total ) ) {
		return qfalse;
	}
	*reserve = total / 100 * MAX( com_logFreeSpace->integer, 0 );
	return qtrue;
}

/*
=============
Log_HasRoom

Whether a log whose write failed is worth opening again
=============
*/
qboolean Log_HasRoom( void ) {
	unsigned long long	available, reserve;

	return !Log_Free( &available, &reserve ) || ( available > LOG_ROOM_BYTES && available >= reserve );
}

/*
=============
Log_Archive

Whether a file name is a finished log's, base-yyyy-mm-dd[.n].log, and
compressed (.gz) or not; if so, its day and number (1 without one)
=============
*/
static qboolean Log_Archive( const char *name, int *day, int *number, qboolean *compressed ) {
	char	stem[MAX_OSPATH];
	char	*p;
	int		length, y, m, d;

	Q_strncpyz( stem, name, sizeof( stem ) );
	*compressed = COM_CompareExtension( stem, ".log.gz" );
	if ( *compressed ) {
		stem[strlen( stem ) - 3] = '\0';
	}
	if ( !COM_CompareExtension( stem, ".log" ) ) {
		return qfalse;
	}
	length = strlen( stem ) - 4;
	stem[length] = '\0';

	// .n
	*number = 1;
	p = stem + length;
	while ( p > stem && p[-1] >= '0' && p[-1] <= '9' ) {
		p--;
	}
	if ( *p && p > stem && p[-1] == '.' ) {
		*number = atoi( p );
		p[-1] = '\0';
		length = p - 1 - stem;
	}

	// -yyyy-mm-dd
	if ( length < 11 ) {
		return qfalse;
	}
	p = stem + length - 11;
	if ( p[0] != '-' || p[5] != '-' || p[8] != '-' || sscanf( p + 1, "%4d-%2d-%2d", &y, &m, &d ) != 3 ||
		m < 1 || m > 12 || d < 1 || d > 31 ) {
		return qfalse;
	}
	*day = y * 10000 + m * 100 + d;
	return *number > 0;
}

/*
=============
Log_Queue

Compresses a finished log, after the others before it
=============
*/
static void Log_Queue( const char *path ) {
	logJob_t	*job;

	if ( numLogJobs == MAX_LOG_JOBS ) {
		// compressed when the program next starts
		return;
	}
	job = &logJobs[( logJobHead + numLogJobs ) % MAX_LOG_JOBS];
	Com_Memset( job, 0, sizeof( *job ) );
	Q_strncpyz( job->path, path, sizeof( job->path ) );
	numLogJobs++;
}

/*
=============
Log_Compress

On its own thread: path becomes path.gz, through path.gz.tmp so a file
cut short never takes its name. zlib here is built without its gzip
wrapper (NO_GZIP), so the header and trailer are written here
=============
*/
static void Log_Compress( void *data ) {
	static const byte	header[10] = { 0x1f, 0x8b, Z_DEFLATED, 0, 0, 0, 0, 0, 0, 255 };
	// one job at a time, so not on the thread's stack
	static byte			in[1 << 16], out[1 << 16];
	logJob_t			*job = data;
	char				tmp[MAX_OSPATH + 8], gz[MAX_OSPATH + 8];
	byte				trailer[8];
	FILE				*from, *to = NULL;
	z_stream			zs;
	uLong				crc = crc32( 0, Z_NULL, 0 );
	size_t				length, written;
	int					flush = Z_NO_FLUSH, i;
	struct stat			st;
	struct utimbuf		times;

	Com_sprintf( gz, sizeof( gz ), "%s.gz", job->path );
	Com_sprintf( tmp, sizeof( tmp ), "%s.gz.tmp", job->path );
	Com_Memset( &zs, 0, sizeof( zs ) );
	errno = 0;
	if ( !( from = Sys_FOpen( job->path, "rb" ) ) ) {
		job->error = errno ? errno : EIO;
		return;
	}
	if ( !( to = Sys_FOpen( tmp, "wb" ) ) ||
		deflateInit2( &zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY ) != Z_OK ) {
		job->error = to ? -1 : ( errno ? errno : EIO );
		goto done;
	}

	if ( fwrite( header, 1, sizeof( header ), to ) != sizeof( header ) ) {
		job->error = errno ? errno : EIO;
	}
	job->outBytes = sizeof( header );
	while ( !job->error && flush != Z_FINISH ) {
		length = fread( in, 1, sizeof( in ), from );
		if ( ferror( from ) ) {
			job->error = errno ? errno : EIO;
			break;
		}
		crc = crc32( crc, in, length );
		job->inBytes += length;
		flush = feof( from ) ? Z_FINISH : Z_NO_FLUSH;
		zs.next_in = in;
		zs.avail_in = length;
		do {
			zs.next_out = out;
			zs.avail_out = sizeof( out );
			if ( deflate( &zs, flush ) == Z_STREAM_ERROR ) {
				job->error = -1;
				break;
			}
			written = sizeof( out ) - zs.avail_out;
			if ( fwrite( out, 1, written, to ) != written ) {
				job->error = errno ? errno : EIO;
			}
			job->outBytes += written;
		} while ( !job->error && zs.avail_out == 0 );
	}
	deflateEnd( &zs );

	// the uncompressed data's CRC-32 and length, little-endian
	for ( i = 0; i < 4; i++ ) {
		trailer[i] = ( crc >> ( 8 * i ) ) & 0xff;
		trailer[4 + i] = ( job->inBytes >> ( 8 * i ) ) & 0xff;
	}
	if ( !job->error && fwrite( trailer, 1, sizeof( trailer ), to ) != sizeof( trailer ) ) {
		job->error = errno ? errno : EIO;
	}
	job->outBytes += sizeof( trailer );

done:
	fclose( from );
	if ( to && fclose( to ) && !job->error ) {
		job->error = errno ? errno : EIO;
	}
	if ( !job->error && !Sys_Rename( tmp, gz ) ) {
		job->error = errno ? errno : EIO;
	}
	if ( job->error ) {
		remove( tmp );
	} else {
		// as gzip does, the compressed log keeps its time, the day it was
		// last written, which com_logDays goes by
		if ( stat( job->path, &st ) == 0 ) {
			times.actime = st.st_atime;
			times.modtime = st.st_mtime;
			utime( gz, &times );
		}
		remove( job->path );
	}
}

/*
=============
Log_StartNext

Starts compressing the next finished log, if none is
=============
*/
static void Log_StartNext( void ) {
	logJob_t	*job;

	if ( logThread || !numLogJobs ) {
		return;
	}
	job = &logJobs[logJobHead];
	job->startTime = Sys_Milliseconds();
	logThread = Sys_StartThread( Log_Compress, job );
	if ( !logThread ) {
		// kept as it is: compressed when the program next starts, or, in
		// a browser, which has no threads to do it without stalling a
		// frame, never
		logJobHead = ( logJobHead + 1 ) % MAX_LOG_JOBS;
		numLogJobs--;
	}
}

/*
=============
Log_AddDir

A directory holding a log; the first time, the finished logs left there
uncompressed, or half compressed, by a run that stopped are taken up
=============
*/
static void Log_AddDir( const char *dir ) {
	char		**names, path[MAX_OSPATH];
	int			i, count, day, number;
	qboolean	compressed;

	for ( i = 0; i < numLogDirs; i++ ) {
		if ( !Q_stricmp( logDirs[i], dir ) ) {
			return;
		}
	}
	if ( numLogDirs == MAX_LOG_DIRS ) {
		return;
	}
	Q_strncpyz( logDirs[numLogDirs++], dir, sizeof( logDirs[0] ) );

	names = Sys_ListFiles( dir, "", NULL, &count, qfalse );
	for ( i = 0; i < count; i++ ) {
		Com_sprintf( path, sizeof( path ), "%s%c%s", dir, PATH_SEP, names[i] );
		if ( COM_CompareExtension( names[i], ".log.gz.tmp" ) ) {
			FS_Remove( path );
		} else if ( Log_Archive( names[i], &day, &number, &compressed ) && !compressed ) {
			Log_Queue( path );
		}
	}
	Sys_FreeFileList( names );
	Log_StartNext();
}

/*
=============
Log_Start

The day a log opened in this run started, its slot made where it's new
=============
*/
static logStart_t *Log_Start( const char *ospath ) {
	logStart_t	*start;
	int			i;

	for ( i = 0; i < MAX_LOG_STARTS; i++ ) {
		if ( logStarts[i].day && !Q_stricmp( logStarts[i].path, ospath ) ) {
			return &logStarts[i];
		}
	}
	start = &logStarts[nextLogStart];
	nextLogStart = ( nextLogStart + 1 ) % MAX_LOG_STARTS;
	Q_strncpyz( start->path, ospath, sizeof( start->path ) );
	start->day = 0;
	return start;
}

/*
=============
Log_CopyAndEmpty

Where Windows won't rename a log another program holds open: it's copied,
then emptied in place
=============
*/
static qboolean Log_CopyAndEmpty( const char *from, const char *to ) {
	static byte	buffer[1 << 16];
	FILE		*in, *out;
	size_t		length;
	qboolean	failed;

	if ( !( in = Sys_FOpen( from, "rb" ) ) ) {
		return qfalse;
	}
	if ( !( out = Sys_FOpen( to, "wb" ) ) ) {
		fclose( in );
		return qfalse;
	}
	while ( ( length = fread( buffer, 1, sizeof( buffer ), in ) ) > 0 && fwrite( buffer, 1, length, out ) == length ) {
	}
	failed = ferror( in ) || length > 0;
	fclose( in );
	if ( fclose( out ) || failed || !( in = Sys_FOpen( from, "wb" ) ) ) {
		FS_Remove( to );
		return qfalse;
	}
	fclose( in );
	return qtrue;
}

/*
=============
Log_Taken

Whether a finished log's name is taken, compressed or not
=============
*/
static qboolean Log_Taken( const char *archive ) {
	char		name[MAX_OSPATH + 8];
	struct stat	st;

	if ( stat( archive, &st ) == 0 ) {
		return qtrue;
	}
	Com_sprintf( name, sizeof( name ), "%s.gz", archive );
	if ( stat( name, &st ) == 0 ) {
		return qtrue;
	}
	Com_sprintf( name, sizeof( name ), "%s.gz.tmp", archive );
	return stat( name, &st ) == 0;
}

/*
=============
Log_Prepare

Before a log is opened to append to: one that started on a day before
today, or has grown past Log_SplitBytes, is set aside, under its first
day's name, and compressed, so the log starts afresh. A rename refused, as
Windows refuses one for a file another program holds open, is copied
instead as a map loads; otherwise the log goes on and is set aside later.
Calls nothing that uses va()'s buffers, which may hold the caller's path
=============
*/
void Log_Prepare( const char *ospath ) {
	char		dir[MAX_OSPATH], base[MAX_OSPATH], archive[MAX_OSPATH], number[16];
	struct stat	st;
	logStart_t	*start = Log_Start( ospath );
	int			today = Log_Day( time( NULL ) );
	int			n;

	Q_strncpyz( dir, ospath, sizeof( dir ) );
	Log_AddDir( Sys_Dirname( dir ) );

	if ( stat( ospath, &st ) != 0 || st.st_size == 0 ) {
		start->day = today;
		return;
	}
	if ( !start->day ) {
		// from an earlier run: the day it was last written
		start->day = Log_Day( st.st_mtime );
	}
	if ( start->day == today && st.st_size <= Log_SplitBytes() ) {
		return;
	}

	// base-yyyy-mm-dd.log, or .2, .3 and on where that's taken; a name too
	// long for that goes on as it is
	COM_StripExtension( ospath, base, sizeof( base ) );
	if ( strlen( base ) + 32 >= sizeof( archive ) ) {
		return;
	}
	for ( n = 1; ; n++ ) {
		if ( n > 9999 ) {
			return;
		}
		Com_sprintf( number, sizeof( number ), n > 1 ? ".%d" : "", n );
		Com_sprintf( archive, sizeof( archive ), "%s-%04d-%02d-%02d%s.log", base,
			start->day / 10000, start->day / 100 % 100, start->day % 100, number );
		if ( !Log_Taken( archive ) ) {
			break;
		}
	}

	if ( !Sys_Rename( ospath, archive ) && !( logMapLoading && Log_CopyAndEmpty( ospath, archive ) ) ) {
		return;
	}
	start->day = today;
	Log_Queue( archive );
	Log_StartNext();
}

typedef struct {
	const char	*name;			// in its directory's Sys_ListFiles
	int			dir, day, number;
	int			written;		// the day it was last written
	long long	bytes;
} logArchive_t;

static int QDECL Log_CompareArchives( const void *a, const void *b ) {
	const logArchive_t	*x = a, *y = b;

	if ( x->day != y->day ) {
		return x->day < y->day ? -1 : 1;
	}
	return x->number - y->number;
}

/*
=============
Log_Prune

Deletes the oldest finished logs while all the logs together are over
com_logSpace, or the disk has less free than com_logFreeSpace, and every
finished log that started, and was last written, more than com_logDays
days before today: one kept open for weeks, by a server that loaded no
map, is set aside under the day it started, and its last lines are new.
A log named by a day after today, as after the clock was set back, isn't
old.
With none left to delete and the disk still under com_logFreeSpace, logs
stop being written until a map load finds it over it again
=============
*/
static void Log_Prune( void ) {
	static qboolean		pruning;	// not again from its own prints
	logArchive_t		*archives = NULL;
	long long			budget = (long long)com_logSpace->integer * ( 1 << 20 );
	long long			total = 0;
	unsigned long long	available = 0, reserve = 0;
	char				**names[MAX_LOG_DIRS], path[MAX_OSPATH];
	int					i, j, count[MAX_LOG_DIRS], numNames = 0, numArchives = 0, oldest = 0;
	time_t				cutoff;
	qboolean			compressed, known, below, over, old;
	struct stat			st;

	logNextDay = Log_DaysFrom( 1 );
	if ( pruning || !numLogDirs ) {
		return;
	}
	pruning = qtrue;

	for ( i = 0; i < numLogDirs; i++ ) {
		names[i] = Sys_ListFiles( logDirs[i], "", NULL, &count[i], qfalse );
		numNames += count[i];
	}
	if ( numNames ) {
		archives = Z_Malloc( numNames * sizeof( *archives ) );
	}
	for ( i = 0; i < numLogDirs; i++ ) {
		for ( j = 0; j < count[i]; j++ ) {
			logArchive_t	*a = &archives[numArchives];
			qboolean		archive = Log_Archive( names[i][j], &a->day, &a->number, &compressed );

			// logs, and the one being compressed, count; only finished
			// ones, not being compressed, may be deleted
			if ( !archive && !COM_CompareExtension( names[i][j], ".log" ) &&
				!COM_CompareExtension( names[i][j], ".log.gz.tmp" ) ) {
				continue;
			}
			Com_sprintf( path, sizeof( path ), "%s%c%s", logDirs[i], PATH_SEP, names[i][j] );
			if ( stat( path, &st ) != 0 ) {
				continue;
			}
			total += st.st_size;
			if ( archive && !( logThread && !Q_stricmp( path, logJobs[logJobHead].path ) ) ) {
				a->name = names[i][j];
				a->dir = i;
				a->written = Log_Day( st.st_mtime );
				a->bytes = st.st_size;
				numArchives++;
			}
		}
	}
	if ( numArchives ) {
		qsort( archives, numArchives, sizeof( archives[0] ), Log_CompareArchives );
	}

	// the first day kept; with no day known, none is too old
	if ( com_logDays->integer > 0 && ( cutoff = Log_DaysFrom( -com_logDays->integer ) ) != (time_t)-1 ) {
		oldest = Log_Day( cutoff );
	}
	known = Log_Free( &available, &reserve );
	for ( i = 0; i < numArchives; i++ ) {
		below = known && available < reserve;
		over = budget > 0 && total > budget;
		old = archives[i].day < oldest && archives[i].written < oldest;
		// a log started long ago may have been written lately, so past the
		// space's oldest, an old one may still come
		if ( !over && !below && !old ) {
			if ( !oldest ) {
				break;
			}
			continue;
		}
		Com_sprintf( path, sizeof( path ), "%s%c%s", logDirs[archives[i].dir], PATH_SEP, archives[i].name );
		FS_Remove( path );
		total -= archives[i].bytes;
		available += archives[i].bytes;
		if ( old ) {
			Com_Printf( "Deleted %s, a log last written more than %d days ago (com_logDays)\n",
				path, com_logDays->integer );
		} else if ( below ) {
			Com_Printf( "Deleted %s, the oldest log, to keep %d%% of the disk free (com_logFreeSpace)\n",
				path, com_logFreeSpace->integer );
		} else {
			Com_Printf( "Deleted %s, the oldest log, to keep the logs within %d MB (com_logSpace)\n",
				path, com_logSpace->integer );
		}
	}

	if ( archives ) {
		Z_Free( archives );
	}
	for ( i = 0; i < numLogDirs; i++ ) {
		Sys_FreeFileList( names[i] );
	}

	// measured again: a deletion may not have freed what it took
	below = Log_Free( &available, &reserve ) && available < reserve;
	if ( below && !logPaused ) {
		time_t	now = time( NULL );
		char	warning[MAX_OSPATH + 256];

		logPaused = qtrue;
		strftime( logPausedAt, sizeof( logPausedAt ), "%Y-%m-%d %H:%M:%S", localtime( &now ) );
		Com_sprintf( warning, sizeof( warning ), S_COLOR_YELLOW "WARNING: the disk holding %s has "
			"less than %d%% free (com_logFreeSpace) and no old logs left to delete, so no log is "
			"written until it has; it's checked again at each map\n", logDirs[0], com_logFreeSpace->integer );
		Sys_Print( warning );
	} else if ( !below && logPaused ) {
		logPaused = qfalse;
		Com_Printf( "Logs are written again: none were from %s, while the disk had less than %d%% "
			"free (com_logFreeSpace)\n", logPausedAt, com_logFreeSpace->integer );
	}
	pruning = qfalse;
}

/*
=============
Log_Check

As the logs are written: old logs are deleted only once the disk is short
of com_logFreeSpace, the rest waiting for a map load or a compressed log
=============
*/
void Log_Check( void ) {
	unsigned long long	available, reserve;

	if ( !logPaused && Log_Free( &available, &reserve ) && available < reserve ) {
		Log_Prune();
	}
}

/*
=============
Log_MapLoad

A map loads: old logs are deleted to keep within their budget, and logs
may start afresh, copied where Windows refuses a rename, until the frame's
end
=============
*/
void Log_MapLoad( void ) {
	logMapLoading = qtrue;
	Log_Prune();
}

/*
=============
Log_Paused

Whether logs aren't written, the disk short of com_logFreeSpace
=============
*/
qboolean Log_Paused( void ) {
	return logPaused;
}

/*
=============
Log_Frame

A compressed log's result, and the next one started; and at a day's
turn, the logs too old for com_logDays deleted
=============
*/
void Log_Frame( void ) {
	logJob_t	*job;
	char		path[MAX_OSPATH];

	logMapLoading = qfalse;
	// a new day deletes the logs past com_logDays, also on a server that
	// loads no map for days, as one hibernating; and after the clock was
	// set back more than a day, a midnight long to come is found again
	if ( logNextDay != (time_t)-1 ) {
		time_t	now = time( NULL );

		if ( now >= logNextDay || logNextDay - now > 2 * 24 * 60 * 60 ) {
			Log_Prune();
		}
	}
	if ( !logThread || !Sys_ThreadFinished( logThread ) ) {
		return;
	}
	logThread = NULL;
	job = &logJobs[logJobHead];
	if ( !job->error ) {
		Q_strncpyz( path, job->path, sizeof( path ) );
		Com_Printf( "Compressed %s, %lld KB, to %lld KB in %d ms\n", Sys_Basename( path ),
			job->inBytes >> 10, job->outBytes >> 10, Sys_Milliseconds() - job->startTime );
	} else if ( job->error != ENOENT ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: couldn't compress %s (%s), so it's kept as it is\n",
			job->path, job->error > 0 ? strerror( job->error ) : "zlib failed" );
	}
	logJobHead = ( logJobHead + 1 ) % MAX_LOG_JOBS;
	numLogJobs--;
	Log_Prune();
	Log_StartNext();
}

/*
=============
Log_Init
=============
*/
void Log_Init( void ) {
	com_logSpace = Cvar_Get( "com_logSpace", "1024", CVAR_ARCHIVE );
	Cvar_CheckRange( com_logSpace, 0, 1048576, qtrue );
	Cvar_SetDescription( com_logSpace, "Megabytes all logs and their old files may take; 0 for no limit\n"
		"The oldest old files are deleted to stay within it: a dedicated server's in its servers/<port> "
		"folder, a player's in the game's folder. A log starts a new file past a quarter of it, or 64 MB" );
	com_logFreeSpace = Cvar_Get( "com_logFreeSpace", "5", CVAR_ARCHIVE );
	Cvar_CheckRange( com_logFreeSpace, 0, 50, qtrue );
	Cvar_SetDescription( com_logFreeSpace, "Percent of the disk the logs leave free; 0 for none\n"
		"Old logs are deleted, oldest first, to keep it free; with none left, no log is written "
		"until a map loads with it free again" );
	com_logDays = Cvar_Get( "com_logDays", "30", CVAR_ARCHIVE );
	Cvar_CheckRange( com_logDays, 0, 36500, qtrue );
	Cvar_SetDescription( com_logDays, "Days old logs are kept; 0 for no limit\n"
		"An old log started and last written more than this many days before today is deleted as a "
		"map loads, a log is compressed or the day turns. The logs being written are kept" );
}
