# Unix specific settings (this includes macOS and emscripten)

if(NOT UNIX)
    return()
endif()

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_unix.c)

if(EMSCRIPTEN)
    list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_passive.c)
else()
    list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_tty.c)
endif()

if(USE_HTTP)
    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/client/cl_http_curl.c)
endif()

list(APPEND COMMON_LIBRARIES
    ${CMAKE_DL_LIBS}    # Dynamic loader
    m                   # Math library
)

# Sys_StartThread, which the web build, without threads, goes without
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    list(APPEND CLIENT_LIBRARIES Threads::Threads)
    list(APPEND SERVER_LIBRARIES Threads::Threads)
endif()
