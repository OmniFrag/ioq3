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
# The installers' name (installer.cmake): the package's file name, and on
# macOS the disk image's
set_identity(PACKAGE_NAME ${PROJECT_NAME})
# CLIENT_WINDOW_TITLE, the name on the window and in the taskbar; PRODUCT_NAME,
# the name in the version string; and HOMEPATH_NAME, the directory in the
# user's home that holds configs and downloads, default to the ones in
# q_shared.h. The web client's page title follows the window title.
# A product that succeeds another may set HOMEPATH_NAME_PREDECESSOR, the
# other's HOMEPATH_NAME (Quake3), and HOMEPATH_NAME_PREDECESSOR_UNIX, its
# HOMEPATH_NAME_UNIX_LEGACY (.q3a) on Linux: its players' paks there are read,
# and its config copied on the first run (files.c).
if(DEFINED CLIENT_WINDOW_TITLE)
    set_identity(WEB_PAGE_TITLE ${CLIENT_WINDOW_TITLE})
else()
    set_identity(WEB_PAGE_TITLE "${CLIENT_NAME} Emscripten demo")
endif()
# The client's icon as a PNG, for the web page and its web app manifest
set_identity(PNG_ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/misc/linux/quake3-tango.png)

set(BASEGAME baseq3)

set(CGAME_MODULE cgame)
set(GAME_MODULE qagame)
set(UI_MODULE ui)

set_identity(WINDOWS_ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/misc/windows/quake3.ico)

# The client's name as the desktop shows it, apart from the window: the web
# app's, and the macOS app's by default
if(DEFINED CLIENT_WINDOW_TITLE)
    set_identity(APP_NAME ${CLIENT_WINDOW_TITLE})
else()
    set_identity(APP_NAME ${CLIENT_NAME})
endif()

set_identity(MACOS_ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/misc/macos/quake3_flat.icns)
set_identity(MACOS_BUNDLE_ID org.ioquake.${CLIENT_NAME})
# The app's name in the menu bar and About box, where SDL also uses
# CLIENT_WINDOW_TITLE. MACOS_BUNDLE_SHORT_VERSION and MACOS_BUNDLE_VERSION
# may be set too (macos.cmake), and default to PRODUCT_VERSION.
set_identity(MACOS_BUNDLE_NAME ${APP_NAME})
# The .app's name in Finder and the Dock, and its executable's
set_identity(MACOS_APP_NAME ${CLIENT_NAME})
# The client's reverse-DNS ID, which desktops match to its .desktop file and
# metainfo (misc/linux) to find its icon and name
set_identity(APP_ID org.ioquake3.${CLIENT_NAME})

set_identity(COPYRIGHT "QUAKE III ARENA Copyright © 1999-2000 id Software, Inc. All rights reserved.")

set_identity(CONTACT_EMAIL "info@ioquake.org")
set(PROTOCOL_HANDLER_SCHEME quake3)
