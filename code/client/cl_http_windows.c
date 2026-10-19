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

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>

// WinINet's calls block, connecting and reading, so a download runs on a
// thread of its own, which hands what it reads to the game's frame through
// a buffer. A download that's stopped is left to its thread, which frees it
// once its blocked call returns; closing its session ends that call early.
//
// The thread follows redirects itself, up to HTTP_MAX_REDIRECTS, checking
// each URL's scheme and the addresses its host resolves to by the client's
// rule (NET_AdrReachableFrom), and then the address WinINet connected to,
// which its status callback reports. WinINet resolves the name again
// itself, so a name that changes its address in between (DNS rebinding)
// can reach a refused address. The request has been sent by then, but its
// answer is never read.

#define HTTP_BUFFER_SIZE	( 1024 * 1024 )	// read and not yet written
#define HTTP_CHUNK_SIZE		( 64 * 1024 )	// one read's most
#define HTTP_MAX_REDIRECTS	5

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
    netScope_t          serverScope;    // the network the server is on
    char                refused[NI_MAXHOST + 128];  // why an address connected to isn't allowed
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
CL_HTTP_AddressAllowed

On the thread: whether an address may be fetched from, with the server's
network as it was when the download began. text gets the address
=================
*/
static qboolean CL_HTTP_AddressAllowed(const httpDownload_t *d, const struct sockaddr *sa, size_t length,
                                       char *text, int size)
{
    netadr_t adr;

    // not NET_AdrToString, whose buffer the frame shares
    if (getnameinfo(sa, (socklen_t)length, text, size, NULL, 0, NI_NUMERICHOST) != 0)
        Q_strncpyz(text, "an address", size);

    return NET_SockaddrToAdr(sa, length, &adr) && NET_AdrReachableFrom(&adr, d->serverScope);
}

/*
=================
CL_HTTP_Status

WinINet's status callback, on the thread: notes an address connected to
that isn't allowed. The address comes as a SOCKADDR, as the documentation
has it, or as a string
=================
*/
static void CALLBACK CL_HTTP_Status(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID info, DWORD length)
{
    httpDownload_t *d = (httpDownload_t *)context;
    const struct sockaddr *sa = info;
    struct addrinfo hints, *numeric = NULL;
    char text[NI_MAXHOST], string[NI_MAXHOST], *address = string, *end;

    if (!d || status != INTERNET_STATUS_CONNECTED_TO_SERVER || !info || length < 2)
        return;

    // a SOCKADDR may come in a larger SOCKADDR_STORAGE
    if (!(sa->sa_family == AF_INET && length >= sizeof(struct sockaddr_in)) &&
        !(sa->sa_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)))
    {
        // a string, with or without its nul in the length given; one that
        // isn't an address is refused, rather than left unchecked
        Q_strncpyz(string, info, length < sizeof(string) ? length + 1 : sizeof(string));
        // an IPv6 address may come in brackets
        if (string[0] == '[' && (end = strchr(string, ']')) != NULL)
        {
            *end = '\0';
            address = string + 1;
        }
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = AI_NUMERICHOST;
        if (getaddrinfo(address, NULL, &hints, &numeric) != 0 || !numeric)
        {
            Q_strncpyz(d->refused, "an address WinINet reported in a form it couldn't check", sizeof(d->refused));
            return;
        }
        sa = numeric->ai_addr;
        length = (DWORD)numeric->ai_addrlen;
    }

    if (!CL_HTTP_AddressAllowed(d, sa, length, text, sizeof(text)))
    {
        // not Com_sprintf, which may print
        Q_strncpyz(d->refused, text, sizeof(d->refused));
        Q_strcat(d->refused, sizeof(d->refused), ", on a private network, and this server isn't on one");
    }
    if (numeric)
        freeaddrinfo(numeric);
}

/*
=================
CL_HTTP_URLAllowed

On the thread: whether a URL is http or https, to a host whose addresses
may all be fetched from; if not, the download fails, saying why. A host
that doesn't resolve is left to WinINet, which says so
=================
*/
static qboolean CL_HTTP_URLAllowed(httpDownload_t *d, const char *url)
{
    URL_COMPONENTSA parts;
    char host[INTERNET_MAX_HOST_NAME_LENGTH];
    char text[NI_MAXHOST];
    struct addrinfo hints, *addresses = NULL, *a;
    qboolean allowed = qtrue;

    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = sizeof(host);
    if (!InternetCrackUrlA(url, 0, 0, &parts) || !host[0])
    {
        CL_HTTP_Fail(d, "%s isn't a URL", url);
        return qfalse;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        CL_HTTP_Fail(d, "%s isn't an http or https URL", url);
        return qfalse;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0)
        return qtrue;
    for (a = addresses; a && allowed; a = a->ai_next)
    {
        if (!CL_HTTP_AddressAllowed(d, a->ai_addr, a->ai_addrlen, text, sizeof(text)))
        {
            CL_HTTP_Fail(d, "refused %s, which leads to %s, on a private network, and this server "
                         "isn't on one", host, text);
            allowed = qfalse;
        }
    }
    freeaddrinfo(addresses);
    return allowed;
}

/*
=================
CL_HTTP_Open

On the thread: requests the download's URL, following its redirects, and
returns the response to read, with a status of 200; NULL if it failed,
saying why
=================
*/
static HINTERNET CL_HTTP_Open(httpDownload_t *d)
{
    HINTERNET hUrl, session;
    DWORD httpCode, len, zero;
    int redirects;
    qboolean stopped;
    char url[MAX_STRING_CHARS], location[MAX_STRING_CHARS];

    Q_strncpyz(url, d->url, sizeof(url));
    for (redirects = 0;; redirects++)
    {
        // the frame may have stopped it, and closed its session, since
        EnterCriticalSection(&d->lock);
        stopped = d->stopped;
        session = d->session;
        LeaveCriticalSection(&d->lock);
        if (stopped || !session)
            return NULL;

        if (!CL_HTTP_URLAllowed(d, url))
            return NULL;

        hUrl = InternetOpenUrlA(session, url, d->headers, (DWORD)-1,
                                INTERNET_FLAG_HYPERLINK |
                                    INTERNET_FLAG_NO_AUTO_REDIRECT |
                                    INTERNET_FLAG_NO_CACHE_WRITE |
                                    INTERNET_FLAG_NO_COOKIES |
                                    INTERNET_FLAG_NO_UI |
                                    INTERNET_FLAG_RESYNCHRONIZE |
                                    INTERNET_FLAG_RELOAD,
                                (DWORD_PTR)d);
        if (d->refused[0])
        {
            CL_HTTP_Fail(d, "refused to read from %s", d->refused);
            break;
        }
        if (!hUrl)
        {
            CL_HTTP_Fail(d, "couldn't connect (WinINet error %lu)", GetLastError());
            return NULL;
        }

        len = sizeof(httpCode);
        zero = 0;
        if (!HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &httpCode, &len, &zero))
        {
            CL_HTTP_Fail(d, "no HTTP status (WinINet error %lu)", GetLastError());
            break;
        }
        if (httpCode == 200)
            return hUrl;
        if (httpCode != 301 && httpCode != 302 && httpCode != 303 && httpCode != 307 && httpCode != 308)
        {
            CL_HTTP_Fail(d, "HTTP code %lu", httpCode);
            break;
        }

        if (redirects == HTTP_MAX_REDIRECTS)
        {
            CL_HTTP_Fail(d, "more than %d redirects", HTTP_MAX_REDIRECTS);
            break;
        }
        len = sizeof(location);
        zero = 0;
        if (!HttpQueryInfoA(hUrl, HTTP_QUERY_LOCATION, location, &len, &zero))
        {
            CL_HTTP_Fail(d, "HTTP code %lu without a Location", httpCode);
            break;
        }
        InternetCloseHandle(hUrl);

        // a Location may be relative to the URL it came from
        len = sizeof(d->chunk);
        if (!InternetCombineUrlA(url, location, (char *)d->chunk, &len, ICU_NO_ENCODE) || len >= sizeof(url))
        {
            CL_HTTP_Fail(d, "a redirect to %s", location);
            return NULL;
        }
        Q_strncpyz(url, (char *)d->chunk, sizeof(url));
    }

    if (hUrl)
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
        d->size = contentLength > INT_MAX ? INT_MAX : (int)contentLength;
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
    d->serverScope = NET_AdrScope(&clc.serverAddress);
    download = d;

    d->session = InternetOpenA(httpAgent, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!d->session)
    {
        CL_HTTP_Fail(d, "couldn't start (WinINet error %lu)", GetLastError());
        goto failed;
    }
    InternetSetStatusCallbackA(d->session, CL_HTTP_Status);

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
