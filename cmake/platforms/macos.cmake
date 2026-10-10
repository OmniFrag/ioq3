# macOS specific settings

if(NOT APPLE)
    return()
endif()

option(BUILD_MACOS_APP "Deploy as a macOS .app" ON)

enable_language(OBJC)

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_osx.m)

list(APPEND COMMON_LIBRARIES "-framework Cocoa")
list(APPEND CLIENT_LIBRARIES "-framework IOKit")
# the dedicated server's power assertion, which keeps the Mac awake for
# its players (Sys_KeepAwake)
list(APPEND SERVER_LIBRARIES "-framework IOKit")
list(APPEND RENDERER_LIBRARIES "-framework OpenGL")

set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
set(CMAKE_OSX_ARCHITECTURES arm64;x86_64)

if(BUILD_MACOS_APP)
    set(CLIENT_EXECUTABLE_OPTIONS MACOSX_BUNDLE)
    list(APPEND POST_CONFIGURE_FUNCTIONS finish_macos_app)
endif()

function(finish_macos_app)
    get_filename_component(MACOS_ICON_FILE ${MACOS_ICON_PATH} NAME)

    set(MACOS_APP_BUNDLE_NAME ${MACOS_BUNDLE_NAME})
    set(MACOS_APP_EXECUTABLE_NAME ${MACOS_APP_NAME})
    set(MACOS_APP_GUI_IDENTIFIER ${MACOS_BUNDLE_ID})
    set(MACOS_APP_ICON_FILE ${MACOS_ICON_FILE})
    # macOS wants numbers here, a version like 1.2.3 and a build number that
    # grows, which a parent project may give (identity.cmake)
    set_identity(MACOS_BUNDLE_SHORT_VERSION ${PRODUCT_VERSION})
    set_identity(MACOS_BUNDLE_VERSION ${PRODUCT_VERSION})
    set(MACOS_APP_SHORT_VERSION_STRING ${MACOS_BUNDLE_SHORT_VERSION})
    set(MACOS_APP_BUNDLE_VERSION ${MACOS_BUNDLE_VERSION})
    set(MACOS_APP_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET})
    set(MACOS_APP_COPYRIGHT ${COPYRIGHT})
    set(MACOS_APP_LOCAL_NETWORK_USAGE ${MACOS_LOCAL_NETWORK_USAGE})

    if(PROTOCOL_HANDLER_SCHEMES)
        list(JOIN PROTOCOL_HANDLER_SCHEMES "</string>
                    <string>" MACOS_APP_URL_SCHEMES)
        set(MACOS_APP_PLIST_URL_TYPES
        "<key>CFBundleURLTypes</key>
        <array>
            <dict>
                <key>CFBundleURLName</key>
                <string>${MACOS_APP_BUNDLE_NAME}</string>
                <key>CFBundleURLSchemes</key>
                <array>
                    <string>${MACOS_APP_URL_SCHEMES}</string>
                </array>
            </dict>
        </array>")
    else()
        set(MACOS_APP_PLIST_URL_TYPES "")
    endif()

    configure_file(${PROJECT_SOURCE_DIR}/cmake/Info.plist.in
        ${CMAKE_BINARY_DIR}/Info.plist @ONLY)

    set_target_properties(${CLIENT_BINARY} PROPERTIES
        OUTPUT_NAME ${MACOS_APP_NAME}
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist)

    set(RESOURCES_DIR $<TARGET_FILE_DIR:${CLIENT_BINARY}>/../Resources)
    add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RESOURCES_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${MACOS_ICON_PATH} ${RESOURCES_DIR})

    if(USE_RENDERER_DLOPEN)
        set(MACOS_APP_BINARY_DIR ${MACOS_APP_NAME}.app/Contents/MacOS)

        if(BUILD_RENDERER_GL1)
            set_output_dirs(${RENDERER_GL1_BINARY} SUBDIRECTORY ${MACOS_APP_BINARY_DIR})
            add_dependencies(${CLIENT_BINARY} ${RENDERER_GL1_BINARY})
        endif()

        if(BUILD_RENDERER_GL2)
            set_output_dirs(${RENDERER_GL2_BINARY} SUBDIRECTORY ${MACOS_APP_BINARY_DIR})
            add_dependencies(${CLIENT_BINARY} ${RENDERER_GL2_BINARY})
        endif()
    endif()
endfunction()

# The package's files are signed where CPack stages them, after they're
# installed, so nothing changes them afterwards, and then the disk image
# (cmake/utils/macos_codesign.cmake): as APPLE_CERTIFICATE_ID when it's
# set, or else ad hoc
set(CPACK_PRE_BUILD_SCRIPTS ${PROJECT_SOURCE_DIR}/cmake/utils/macos_codesign.cmake)
set(CPACK_POST_BUILD_SCRIPTS ${PROJECT_SOURCE_DIR}/cmake/utils/macos_codesign.cmake)

set(CPACK_GENERATOR "DragNDrop")

# Files for the disk image, beside the app and the server, by absolute
# path: a project's README template (MACOS_PACKAGE_README), installed as
# README.txt with its @VERSION@ the build's, and others as they are
# (MACOS_PACKAGE_FILES), its license and notices, say. Their names, as an
# AppleScript list, are MACOS_PACKAGE_FILE_NAMES, for the image's layout
function(add_macos_package_files)
    set(NAMES)
    if(MACOS_PACKAGE_README)
        set(VERSION ${PRODUCT_VERSION})
        configure_file(${MACOS_PACKAGE_README} ${CMAKE_BINARY_DIR}/README.txt @ONLY)
        install(FILES ${CMAKE_BINARY_DIR}/README.txt DESTINATION .)
        list(APPEND NAMES "\"README.txt\"")
    endif()
    foreach(FILE IN LISTS MACOS_PACKAGE_FILES)
        install(FILES ${FILE} DESTINATION .)
        get_filename_component(NAME ${FILE} NAME)
        list(APPEND NAMES "\"${NAME}\"")
    endforeach()
    list(JOIN NAMES ", " NAMES)
    set(MACOS_PACKAGE_FILE_NAMES "{${NAMES}}" PARENT_SCOPE)
endfunction()
add_macos_package_files()

set(CPACK_DMG_VOLUME_NAME "${PACKAGE_NAME} Installer")
set(CPACK_DMG_BACKGROUND_IMAGE "${PROJECT_SOURCE_DIR}/misc/macos/macos-dmg-background.png")
set(CPACK_DMG_SUBDIRECTORY "${MACOS_APP_NAME}")

configure_file(
  "${PROJECT_SOURCE_DIR}/misc/macos/macos-dmg-setup.applescript.in"
  "${CMAKE_BINARY_DIR}/macos-dmg-setup.applescript"
  @ONLY
)

set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${CMAKE_BINARY_DIR}/macos-dmg-setup.applescript")
