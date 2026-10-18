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

static HINTERNET hInternet = NULL;
static HINTERNET hUrl = NULL;

void CL_HTTP_CancelDownload(void);

// why the download failed, for CL_HTTP_PerformDownload to report
static char httpError[1024];

static Q_PRINTF_FUNC(2, 3) qboolean FailIf(qboolean condition, const char *fmt, ...)
{
    char buffer[1024];

    if (!condition)
        return qfalse;

    va_list argptr;
    va_start(argptr, fmt);
    Q_vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);

    Com_sprintf(httpError, sizeof(httpError), "%s from %s", buffer, clc.downloadURL);
    return qtrue;
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

    hInternet = InternetOpenA(
        va("%s %s", Q3_VERSION, windowsVersion),
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);

    return hInternet != NULL;
}

/*
=================
CL_HTTP_Available
=================
*/
qboolean CL_HTTP_Available()
{
    return hInternet != NULL;
}

/*
=================
CL_HTTP_Shutdown
=================
*/
void CL_HTTP_Shutdown(void)
{
    if (hInternet)
    {
        InternetCloseHandle(hInternet);
        hInternet = NULL;
    }
}

/*
=================
CL_HTTP_BeginDownload
=================
*/
void CL_HTTP_BeginDownload(const char *remoteURL)
{
    DWORD httpCode = 0;
    DWORD contentLength = 0;
    DWORD len = sizeof(httpCode);
    DWORD zero = 0;
    BOOL success;

    httpError[0] = '\0';
    hUrl = InternetOpenUrlA(hInternet, remoteURL,
                            va("Referer: ioQ3://%s\r\n", NET_AdrToString(clc.serverAddress)), (DWORD)-1,
                            INTERNET_FLAG_HYPERLINK |
                                INTERNET_FLAG_NO_CACHE_WRITE |
                                INTERNET_FLAG_NO_COOKIES |
                                INTERNET_FLAG_NO_UI |
                                INTERNET_FLAG_RESYNCHRONIZE |
                                INTERNET_FLAG_RELOAD,
                            0);

    if (FailIf(hUrl == NULL, "InternetOpenUrlA failed %lu", GetLastError()))
        return;

    success = HttpQueryInfo(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &httpCode, &len, &zero);
    if (FailIf(!success, "Get HTTP_QUERY_STATUS_CODE failed %lu", GetLastError()))
        return;

    if (FailIf(httpCode >= 400, "HTTP code %lu", httpCode))
        return;
    if (FailIf(httpCode != 200, "Unhandled HTTP code %lu", httpCode))
        return;

    success = HttpQueryInfo(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &len, &zero);
    if (FailIf(!success, "Get HTTP_QUERY_CONTENT_LENGTH failed %lu", GetLastError()))
        return;

    clc.downloadSize = (int)contentLength;
    Cvar_SetValue("cl_downloadSize", clc.downloadSize);
}

/*
=================
CL_HTTP_PerformDownload
=================
*/
qboolean CL_HTTP_PerformDownload(char *error, int errorSize)
{
    static BYTE readBuffer[256 * 1024];
    DWORD bytesRead = 0;
    BOOL success;

    if (!httpError[0])
        FailIf(hUrl == NULL, "hUrl is NULL");
    if (httpError[0])
    {
        Q_strncpyz(error, httpError, errorSize);
        CL_HTTP_CancelDownload();
        return qtrue;
    }
    *error = '\0';

    success = InternetReadFile(hUrl, readBuffer, sizeof(readBuffer), &bytesRead);
    if (FailIf(!success, "InternetReadFile failed %lu", GetLastError()))
        return qfalse;

    if (bytesRead > 0)
    {
        clc.downloadCount += bytesRead;
        Cvar_SetValue("cl_downloadCount", clc.downloadCount);

        DWORD bytesWritten = (DWORD)FS_Write(readBuffer, bytesRead, clc.download);
        FailIf(bytesWritten != bytesRead, "bytesWritten != bytesRead");

        return qfalse;
    }

    InternetCloseHandle(hUrl);
    hUrl = NULL;
    return qtrue;
}

/*
=================
CL_HTTP_CancelDownload
=================
*/
void CL_HTTP_CancelDownload(void)
{
    if (hUrl)
    {
        InternetCloseHandle(hUrl);
        hUrl = NULL;
    }
}

#endif /* USE_HTTP */
