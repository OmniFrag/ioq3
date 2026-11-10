# Emscripten specific settings

if(NOT EMSCRIPTEN)
    return()
endif()

set(CMAKE_EXECUTABLE_SUFFIX ".js")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".wasm")

# Disable options that don't make sense for emscripten
set(BUILD_SERVER OFF CACHE INTERNAL "")
set(BUILD_RENDERER_GL1 OFF CACHE INTERNAL "")
set(USE_RENDERER_DLOPEN OFF CACHE INTERNAL "")
set(USE_OPENAL_DLOPEN OFF CACHE INTERNAL "")
set(BUILD_GAME_LIBRARIES OFF CACHE INTERNAL "")
set(USE_HTTP OFF CACHE INTERNAL "")

# Sound through Web Audio (code/web/snd_web.c): the base backend decides what
# plays, and the browser mixes each sound's node on its audio thread, its
# samples outside the heap. Off, Emscripten's OpenAL plays it, or SDL's
# ScriptProcessorNode with s_useOpenAL 0.
option(USE_WEB_AUDIO "Play sound through Web Audio nodes" ON)
if(USE_WEB_AUDIO)
    # not in the cache, so turning USE_WEB_AUDIO off brings OpenAL back
    set(USE_OPENAL OFF)
endif()

# Disable LTO since the libraries Emscripten provides aren't LTO enabled
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)

# The oldest WebGL the client runs on, which the page also checks for. A
# parent project that needs WebGL 2 may set it to 2 beforehand.
if(NOT DEFINED WEB_MIN_WEBGL_VERSION)
    set(WEB_MIN_WEBGL_VERSION 1)
endif()

list(APPEND CLIENT_LINK_OPTIONS
    -sTOTAL_MEMORY=256MB
    -sSTACK_SIZE=5MB
    -sMIN_WEBGL_VERSION=${WEB_MIN_WEBGL_VERSION}
    -sMAX_WEBGL_VERSION=2
    -sEXPORTED_RUNTIME_METHODS=FS,addRunDependency,removeRunDependency
    -sEXIT_RUNTIME=1
    -sEXPORT_ES6
    -sEXPORT_NAME=${CLIENT_NAME}
    # the page keeps the home in the browser's storage (client.html.in)
    -lidbfs.js
    # the browser's connections to servers (code/web/net_web.c)
    --js-library=${SOURCE_DIR}/web/net_web.js
)

option(EMSCRIPTEN_PRELOAD_FILE "Preload game files into .data file" OFF)

if(EMSCRIPTEN_PRELOAD_FILE)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${BASEGAME}")
        message(FATAL_ERROR "No files in '${BASEGAME}' directory for emscripten to preload.")
    endif()
    list(APPEND CLIENT_LINK_OPTIONS --preload-file "${BASEGAME}")
endif()

list(APPEND POST_CONFIGURE_FUNCTIONS deploy_shell_files)

include(utils/png_size)

function(deploy_shell_files)
    configure_file(${SOURCE_DIR}/web/client.html.in
        ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/${CLIENT_NAME}.html @ONLY)

    # The page's icon, and a web app manifest, with which a browser can
    # install the page as an app
    png_size(${PNG_ICON_PATH} ICON_WIDTH ICON_HEIGHT)
    string(REPLACE "\\" "\\\\" WEB_APP_NAME "${APP_NAME}") # as JSON
    string(REPLACE "\"" "\\\"" WEB_APP_NAME "${WEB_APP_NAME}")
    configure_file(${PNG_ICON_PATH}
        ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/${CLIENT_NAME}.png COPYONLY)
    configure_file(${SOURCE_DIR}/web/client.webmanifest.in
        ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/${CLIENT_NAME}.webmanifest @ONLY)

    if(NOT EMSCRIPTEN_PRELOAD_FILE)
        configure_file(${SOURCE_DIR}/web/client-config.json
            ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/${CLIENT_NAME}-config.json COPYONLY)
    endif()

    # The page checks the player's own paks (pak_check.js) against the game's
    # own checksums, read here from files.c's tables, so they have one
    # source: Quake III Arena 1.32's, Team Arena's and the demo's, by game
    # directory, as JSON. A standalone build has none. They're id's and never
    # change, so an edit to files.c doesn't configure the build again.
    set(WEB_GAME_PAKS "{}")
    if(NOT BUILD_STANDALONE)
        set(FILES_C ${SOURCE_DIR}/qcommon/files.c)
        file(READ ${FILES_C} FILES_C_TEXT)
        foreach(TABLE pak_checksums missionpak_checksums)
            string(REGEX MATCH "[ \t]${TABLE}\\[\\][ \t]*=[ \t\r\n]*[{]([^}]*)[}]" FOUND "${FILES_C_TEXT}")
            string(REGEX MATCHALL "[0-9]+" ${TABLE} "${CMAKE_MATCH_1}")
            list(LENGTH ${TABLE} ${TABLE}_COUNT)
            list(JOIN ${TABLE} "," ${TABLE})
        endforeach()
        string(REGEX MATCH "#define[ \t]+DEMO_PAK0_CHECKSUM[ \t]+([0-9]+)" FOUND "${FILES_C_TEXT}")
        set(DEMO_PAK0_CHECKSUM "${CMAKE_MATCH_1}")
        if(NOT pak_checksums_COUNT EQUAL 9 OR NOT missionpak_checksums_COUNT EQUAL 4 OR NOT DEMO_PAK0_CHECKSUM)
            message(FATAL_ERROR "Couldn't read the game's pak checksums from ${FILES_C} for the web page "
                "(pak_checksums, missionpak_checksums and DEMO_PAK0_CHECKSUM)")
        endif()
        set(WEB_GAME_PAKS
            "{\"${BASEGAME}\":[${pak_checksums}],\"missionpack\":[${missionpak_checksums}],\"demoq3\":[${DEMO_PAK0_CHECKSUM}]}")
    endif()
    configure_file(${SOURCE_DIR}/web/pak_check.js
        ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/pak_check.js @ONLY)
endfunction()
