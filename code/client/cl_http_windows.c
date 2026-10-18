/*
===========================================================================
Copyright (C) 2025 Tim Angus (tim@ngus.net)

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

#ifdef USE_HTTP

#include "client.h"

#include <windows.h>
#include <wininet.h>

// WinINet's calls block, connecting and reading, so a download runs on a
// thread of its own, which hands what it reads to the game's frame through
// a buffer. A download that's stopped is left to its thread, which frees it
// once its blocked call returns; closing its session ends that call early.

#define HTTP_BUFFER_SIZE	( 1024 * 1024 )	// read and not yet written
#define HTTP_CHUNK_SIZE		( 64 * 1024 )	// one read's most

typedef struct {
    LONG                refs;   // the frame's and the thread's
    CRITICAL_SECTION    lock;   // for what follows it
    HINTERNET           session;    // the download's own, closed by the frame
    qboolean            stopped;    // the frame is done with it
    qboolean            finished;   // the thread is done reading
    int                 size;       // Content-Length; 0 when there was none
    int                 length;     // of data
    char                error[1024];    // why it failed, or empty
    byte                data[HTTP_BUFFER_SIZE];
    // the thread's own
    char                url[MAX_STRING_CHARS];
    char                headers[128];
    byte                chunk[HTTP_CHUNK_SIZE];
} httpDownload_t;

static char httpAgent[256];
static httpDownload_t *download = NULL;

/*
=================
CL_HTTP_Release
=================
*/
static void CL_HTTP_Release(httpDownload_t *d)
{
    if (InterlockedDecrement(&d->refs) == 0)
    {
        DeleteCriticalSection(&d->lock);
        free(d);
    }
}

/*
=================
CL_HTTP_Fail

Notes why the download failed; the first reason counts. Calls nothing
that may print, since it runs on the download's thread too
=================
*/
static Q_PRINTF_FUNC(2, 3) void CL_HTTP_Fail(httpDownload_t *d, const char *fmt, ...)
{
    char buffer[1024];
    va_list argptr;

    va_start(argptr, fmt);
    Q_vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);

    EnterCriticalSection(&d->lock);
    if (!d->error[0])
    {
        Q_strncpyz(d->error, buffer, sizeof(d->error));
        Q_strcat(d->error, sizeof(d->error), " from ");
        Q_strcat(d->error, sizeof(d->error), d->url);
    }
    LeaveCriticalSection(&d->lock);
}

/*
=================
CL_HTTP_Open

On the thread: requests the download's URL, and returns the response to
read, with a status of 200; NULL if it failed, saying why
=================
*/
static HINTERNET CL_HTTP_Open(httpDownload_t *d)
{
    HINTERNET hUrl;
    DWORD httpCode, len = sizeof(httpCode), zero = 0;

    hUrl = InternetOpenUrlA(d->session, d->url, d->headers, (DWORD)-1,
                            INTERNET_FLAG_HYPERLINK |
                                INTERNET_FLAG_NO_CACHE_WRITE |
                                INTERNET_FLAG_NO_COOKIES |
                                INTERNET_FLAG_NO_UI |
                                INTERNET_FLAG_RESYNCHRONIZE |
                                INTERNET_FLAG_RELOAD,
                            0);
    if (!hUrl)
    {
        CL_HTTP_Fail(d, "couldn't connect (WinINet error %lu)", GetLastError());
        return NULL;
    }

    if (!HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &httpCode, &len, &zero))
        CL_HTTP_Fail(d, "no HTTP status (WinINet error %lu)", GetLastError());
    else if (httpCode == 200)
        return hUrl;
    else
        CL_HTTP_Fail(d, "HTTP code %lu", httpCode);

    InternetCloseHandle(hUrl);
    return NULL;
}

/*
=================
CL_HTTP_Thread

Connects, then reads into the download's buffer until the end, an error,
or the frame stops it
=================
*/
static DWORD WINAPI CL_HTTP_Thread(LPVOID param)
{
    httpDownload_t *d = param;
    HINTERNET hUrl;
    DWORD contentLength = 0, len = sizeof(contentLength), zero = 0, bytesRead;
    qboolean stopped;
    int room;

    hUrl = CL_HTTP_Open(d);
    if (!hUrl)
        goto done;

    // without a Content-Length, the size shows as unknown, and the pak's
    // checksum still tells a whole file from a cut one
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &len, &zero))
    {
        EnterCriticalSection(&d->lock);
        d->size = (int)contentLength;
        LeaveCriticalSection(&d->lock);
    }

    for (;;)
    {
        EnterCriticalSection(&d->lock);
        stopped = d->stopped;
        room = HTTP_BUFFER_SIZE - d->length;
        LeaveCriticalSection(&d->lock);

        if (stopped)
            break;
        if (room <= 0)
        {
            // the frame writes it out
            Sleep(5);
            continue;
        }

        bytesRead = 0;
        if (!InternetReadFile(hUrl, d->chunk, (DWORD)(room < HTTP_CHUNK_SIZE ? room : HTTP_CHUNK_SIZE), &bytesRead))
        {
            CL_HTTP_Fail(d, "the transfer failed (WinINet error %lu)", GetLastError());
            break;
        }
        if (bytesRead == 0)
            break;

        EnterCriticalSection(&d->lock);
        memcpy(d->data + d->length, d->chunk, bytesRead);
        d->length += bytesRead;
        LeaveCriticalSection(&d->lock);
    }
    InternetCloseHandle(hUrl);

done:
    EnterCriticalSection(&d->lock);
    d->finished = qtrue;
    LeaveCriticalSection(&d->lock);
    CL_HTTP_Release(d);
    return 0;
}

/*
=================
CL_HTTP_Init
=================
*/
qboolean CL_HTTP_Init()
{
    OSVERSIONINFO osvi = {sizeof(OSVERSIONINFO)};
    const char *windowsVersion = GetVersionEx(&osvi) ? va("Windows %lu.%lu (build %lu)",
                                                          osvi.dwMajorVersion,
                                                          osvi.dwMinorVersion,
                                                          osvi.dwBuildNumber)
                                                     : "Windows";

    // each download opens its own session with it
    Com_sprintf(httpAgent, sizeof(httpAgent), "%s %s", Q3_VERSION, windowsVersion);
    return qtrue;
}

/*
=================
CL_HTTP_Available
=================
*/
qboolean CL_HTTP_Available()
{
    return httpAgent[0] != '\0';
}

/*
=================
CL_HTTP_Shutdown
=================
*/
void CL_HTTP_Shutdown(void)
{
    CL_HTTP_CancelDownload();
    httpAgent[0] = '\0';
}

/*
=================
CL_HTTP_BeginDownload
=================
*/
void CL_HTTP_BeginDownload(const char *remoteURL)
{
    httpDownload_t *d;
    HANDLE thread;

    CL_HTTP_CancelDownload();

    d = calloc(1, sizeof(*d));
    if (!d)
    {
        Com_Error(ERR_DROP, "CL_HTTP_BeginDownload: out of memory");
        return;
    }
    InitializeCriticalSection(&d->lock);
    d->refs = 1;
    Q_strncpyz(d->url, remoteURL, sizeof(d->url));
    Com_sprintf(d->headers, sizeof(d->headers), "Referer: ioQ3://%s\r\n", NET_AdrToString(clc.serverAddress));
    download = d;

    d->session = InternetOpenA(httpAgent, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!d->session)
    {
        CL_HTTP_Fail(d, "couldn't start (WinINet error %lu)", GetLastError());
        goto failed;
    }

    InterlockedIncrement(&d->refs);
    thread = CreateThread(NULL, 0, CL_HTTP_Thread, d, 0, NULL);
    if (!thread)
    {
        InterlockedDecrement(&d->refs);
        CL_HTTP_Fail(d, "couldn't start a thread (error %lu)", GetLastError());
        goto failed;
    }
    CloseHandle(thread);
    return;

failed:
    // the next frame reports it
    d->finished = qtrue;
}

/*
=================
CL_HTTP_PerformDownload

Writes out what the thread has read; returns whether the download has
ended, with error empty if it arrived whole. The caller then cancels it
=================
*/
qboolean CL_HTTP_PerformDownload(char *error, int errorSize)
{
    httpDownload_t *d = download;
    qboolean finished;

    *error = '\0';
    if (!d)
    {
        Q_strncpyz(error, "no download under way", errorSize);
        return qtrue;
    }

    // the thread waits for the lock only between its reads
    EnterCriticalSection(&d->lock);
    if (d->length > 0)
    {
        clc.downloadCount += d->length;
        Cvar_SetValue("cl_downloadCount", clc.downloadCount);
        if (FS_Write(d->data, d->length, clc.download) != d->length && !d->error[0])
            Com_sprintf(d->error, sizeof(d->error), "couldn't write %s", clc.downloadTempName);
        d->length = 0;
    }
    if (d->size != clc.downloadSize)
    {
        clc.downloadSize = d->size;
        Cvar_SetValue("cl_downloadSize", clc.downloadSize);
    }
    // the thread had read everything before it finished, and that's
    // written now
    finished = d->finished || d->error[0];
    Q_strncpyz(error, d->error, errorSize);
    LeaveCriticalSection(&d->lock);

    return finished;
}

/*
=================
CL_HTTP_CancelDownload

Stops the download, if one is under way, and leaves it to its thread
=================
*/
void CL_HTTP_CancelDownload(void)
{
    httpDownload_t *d = download;
    HINTERNET session;

    if (!d)
        return;
    download = NULL;

    EnterCriticalSection(&d->lock);
    d->stopped = qtrue;
    session = d->session;
    d->session = NULL;
    LeaveCriticalSection(&d->lock);

    // ends a connect or read under way on the thread
    if (session)
        InternetCloseHandle(session);
    CL_HTTP_Release(d);
}

#endif /* USE_HTTP */
