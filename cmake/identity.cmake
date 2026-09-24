set(PROJECT_NAME ioq3)
set(PROJECT_VERSION 1.36)

# A parent project that includes this one with add_subdirectory() may set the
# set_identity() values beforehand to rebrand the build outputs. The plain
# set() values are also hardcoded in the C code (see q_shared.h).
macro(set_identity NAME VALUE)
    if(NOT DEFINED ${NAME})
        set(${NAME} "${VALUE}")
    endif()
endmacro()

set_identity(SERVER_NAME ioq3ded)
set_identity(CLIENT_NAME ioquake3)
# CLIENT_WINDOW_TITLE, the name on the window and in the taskbar, defaults to
# the one in q_shared.h

set(BASEGAME baseq3)

set(CGAME_MODULE cgame)
set(GAME_MODULE qagame)
set(UI_MODULE ui)

set_identity(WINDOWS_ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/misc/windows/quake3.ico)

set_identity(MACOS_ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/misc/macos/quake3_flat.icns)
set_identity(MACOS_BUNDLE_ID org.ioquake.${CLIENT_NAME})

set_identity(COPYRIGHT "QUAKE III ARENA Copyright © 1999-2000 id Software, Inc. All rights reserved.")

set_identity(CONTACT_EMAIL "info@ioquake.org")
set(PROTOCOL_HANDLER_SCHEME quake3)
