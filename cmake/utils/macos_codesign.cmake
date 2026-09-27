# Signs what's packaged for macOS, as APPLE_CERTIFICATE_ID, the name of a
# Developer ID Application certificate in the keychain. CPack runs it twice
# (macos.cmake): before it makes a package, on what it staged, and after,
# on the disk images it made. It also runs on its own for an install tree:
#
#   cmake -DDIR=<installed files> -P macos_codesign.cmake
#
# The staged files are signed inside out, as Apple asks (codesign's --deep
# is deprecated): each app's libraries, then the app, then the other
# executables, which get the hardened runtime and cmake/entitlements.plist.
# Every signature gets a secure timestamp, which notarization requires.
# With no identity, the files are signed ad hoc, which runs only on the Mac
# that built them, and without the hardened runtime: its library
# validation would refuse the app's own libraries, which have no team to
# match. Disk images are then left unsigned.

cmake_minimum_required(VERSION 3.25)

set(IDENTITY "$ENV{APPLE_CERTIFICATE_ID}")

function(codesign)
    execute_process(COMMAND codesign --force ${ARGN} RESULT_VARIABLE RESULT)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "codesign failed: ${ARGN}")
    endif()
endfunction()

# after CPack made the packages: the disk images, so that Gatekeeper checks
# them when they're opened
if(CPACK_PACKAGE_FILES)
    if(IDENTITY)
        foreach(FILE IN LISTS CPACK_PACKAGE_FILES)
            if(FILE MATCHES "\\.dmg$")
                codesign(--sign "${IDENTITY}" --timestamp "${FILE}")
            endif()
        endforeach()
    endif()
    return()
endif()

if(CPACK_TEMPORARY_INSTALL_DIRECTORY)
    set(DIR "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
endif()
if(NOT IS_DIRECTORY "${DIR}")
    message(FATAL_ERROR "macos_codesign.cmake: no directory to sign (DIR)")
endif()

if(IDENTITY)
    set(SIGN --sign "${IDENTITY}" --timestamp)
    set(EXECUTABLE ${SIGN} --options runtime
        --entitlements "${CMAKE_CURRENT_LIST_DIR}/../entitlements.plist")
    message(STATUS "Signing ${DIR} as ${IDENTITY}")
else()
    set(SIGN --sign -)
    set(EXECUTABLE ${SIGN})
    message(STATUS "Signing ${DIR} ad hoc: APPLE_CERTIFICATE_ID isn't set")
endif()

# each app: its libraries, then the app, which seals them in
file(GLOB_RECURSE APPS LIST_DIRECTORIES true "${DIR}/*.app")
list(FILTER APPS EXCLUDE REGEX "\\.app/")
foreach(APP IN LISTS APPS)
    file(GLOB_RECURSE LIBRARIES "${APP}/*.dylib" "${APP}/*.so")
    foreach(LIBRARY IN LISTS LIBRARIES)
        codesign(${SIGN} "${LIBRARY}")
    endforeach()
    codesign(${EXECUTABLE} "${APP}")
    execute_process(COMMAND codesign --verify --strict --deep "${APP}"
        RESULT_VARIABLE RESULT)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "${APP}'s signature doesn't verify")
    endif()
endforeach()

# the Mach-O executables outside the apps, such as the dedicated server
file(GLOB_RECURSE FILES "${DIR}/*")
list(FILTER FILES EXCLUDE REGEX "\\.app/")
foreach(FILE IN LISTS FILES)
    file(READ "${FILE}" MAGIC LIMIT 4 HEX)
    if(MAGIC MATCHES "^(cafebabe|cffaedfe|cefaedfe)$")
        get_filename_component(NAME "${FILE}" NAME)
        codesign(${EXECUTABLE} --identifier "${NAME}" "${FILE}")
    endif()
endforeach()
