# Windows specific settings

if(NOT WIN32)
    return()
endif()

list(APPEND SYSTEM_PLATFORM_SOURCES
    ${SOURCE_DIR}/sys/sys_win32.c
    ${SOURCE_DIR}/sys/win_resource.rc
)

list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_passive.c)
list(APPEND SERVER_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_win32.c)

if(USE_HTTP)
    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/client/cl_http_windows.c)
    list(APPEND CLIENT_LIBRARIES wininet)
endif()

list(APPEND COMMON_LIBRARIES
    ws2_32 # Windows Sockets 2
    winmm  # timeBeginPeriod/timeEndPeriod
    psapi  # EnumProcesses
)

if(MINGW)
    list(APPEND COMMON_LIBRARIES mingw32)
endif()

list(APPEND CLIENT_DEFINITIONS USE_ICON)

set_source_files_properties(${SOURCE_DIR}/sys/win_resource.rc
    PROPERTIES COMPILE_DEFINITIONS WINDOWS_ICON_PATH="${WINDOWS_ICON_PATH}")

# The executables' version resource, which Explorer shows in their
# properties: APP_NAME, PRODUCT_VERSION, COPYRIGHT, and WINDOWS_VERSION, up
# to four numbers, which a parent project may set, and which otherwise are
# those PRODUCT_VERSION starts with (1.36 of 1.36_g1a2b3c4)
if(NOT DEFINED WINDOWS_VERSION AND PRODUCT_VERSION MATCHES "^v?([0-9]+(\\.[0-9]+)+)")
    set(WINDOWS_VERSION ${CMAKE_MATCH_1})
endif()
string(REPLACE "." ";" WINDOWS_VERSION_NUMBERS "${WINDOWS_VERSION}")
list(APPEND WINDOWS_VERSION_NUMBERS 0 0 0 0)
list(SUBLIST WINDOWS_VERSION_NUMBERS 0 4 WINDOWS_VERSION_NUMBERS)
list(JOIN WINDOWS_VERSION_NUMBERS "," WINDOWS_VERSION_NUMBERS)
# A resource's strings escape a quote by doubling it
string(REPLACE "\"" "\"\"" WINDOWS_APP_NAME "${APP_NAME}")
string(REPLACE "\"" "\"\"" WINDOWS_COPYRIGHT "${COPYRIGHT}")
set(WINDOWS_VERSION_HEADER ${CMAKE_BINARY_DIR}/win_version.h)
file(CONFIGURE OUTPUT ${WINDOWS_VERSION_HEADER} CONTENT [[
#define VERSION_NUMBERS			@WINDOWS_VERSION_NUMBERS@
#define VERSION_STRING			"@PRODUCT_VERSION@"
#define VERSION_APP_NAME		"@WINDOWS_APP_NAME@"
#define VERSION_SERVER_NAME		"@WINDOWS_APP_NAME@ dedicated server"
#define VERSION_COPYRIGHT		"@WINDOWS_COPYRIGHT@"
]] @ONLY)
set_property(SOURCE ${SOURCE_DIR}/sys/win_resource.rc APPEND PROPERTY
    COMPILE_DEFINITIONS WINDOWS_VERSION_HEADER="${WINDOWS_VERSION_HEADER}")
# It's included through a macro, which a build tool's dependency scan may
# not follow
set_property(SOURCE ${SOURCE_DIR}/sys/win_resource.rc APPEND PROPERTY
    OBJECT_DEPENDS ${WINDOWS_VERSION_HEADER})

if(MSVC)
    # We have our own manifest, disable auto creation
    list(APPEND SERVER_LINK_OPTIONS "/MANIFEST:NO")
    list(APPEND CLIENT_LINK_OPTIONS "/MANIFEST:NO")
endif()

set(CLIENT_EXECUTABLE_OPTIONS WIN32)

set(CPACK_GENERATOR NSIS)
set(CPACK_NSIS_MUI_ICON ${WINDOWS_ICON_PATH})
set(CPACK_NSIS_EXECUTABLES_DIRECTORY .)
