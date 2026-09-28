# Linux-like specific settings, i.e. including FreeBSD etc.

if(NOT UNIX OR APPLE)
    return()
endif()

list(APPEND CLIENT_DEFINITIONS USE_ICON)
list(APPEND RENDERER_DEFINITIONS USE_ICON)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE /opt/quake3)
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

# The client's desktop entry, metainfo and icon, named by APP_ID, from which
# desktops and software centers take its name and icon: beside the client
# in the build directory, and installed under share/ in the install prefix,
# where the freedesktop.org specifications keep them
if(BUILD_CLIENT AND NOT EMSCRIPTEN)
    include(utils/png_size)

    set(DESKTOP_FILES_DIR ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE})
    configure_file(${PROJECT_SOURCE_DIR}/misc/linux/client.desktop.in
        ${DESKTOP_FILES_DIR}/${APP_ID}.desktop @ONLY)
    configure_file(${LINUX_METAINFO_PATH} ${DESKTOP_FILES_DIR}/${APP_ID}.metainfo.xml @ONLY)
    configure_file(${PNG_ICON_PATH} ${DESKTOP_FILES_DIR}/${APP_ID}.png COPYONLY)

    png_size(${PNG_ICON_PATH} ICON_WIDTH ICON_HEIGHT)
    install(FILES ${DESKTOP_FILES_DIR}/${APP_ID}.desktop DESTINATION share/applications)
    install(FILES ${DESKTOP_FILES_DIR}/${APP_ID}.metainfo.xml DESTINATION share/metainfo)
    install(FILES ${DESKTOP_FILES_DIR}/${APP_ID}.png
        DESTINATION share/icons/hicolor/${ICON_WIDTH}x${ICON_HEIGHT}/apps)
endif()
