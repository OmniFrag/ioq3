# Signs what's packaged for macOS, as APPLE_CERTIFICATE_ID, the hash or (part
# of the) name of a Developer ID Application certificate in the keychain. CPack
# runs it twice (macos.cmake): before it makes a package, on what it
# staged, and after, on the disk images it made.
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
    execute_process(COMMAND codesign ${ARGN} RESULT_VARIABLE RESULT)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "codesign failed: ${ARGN}")
    endif()
endfunction()

if(IDENTITY)
    # codesign takes an identity's hash, its name, or part of the name
    execute_process(COMMAND security find-identity -v -p codesigning
        OUTPUT_VARIABLE OUTPUT)
    string(REGEX MATCHALL "[0-9A-F]+ \"[^\"\n]*\"" IDENTITIES "${OUTPUT}")
    string(TOUPPER "${IDENTITY}" HASH)
    set(FOUND -1)
    foreach(ENTRY IN LISTS IDENTITIES)
        string(REGEX MATCH "^([0-9A-F]+) \"(.*)\"$" ENTRY "${ENTRY}")
        string(FIND "${CMAKE_MATCH_2}" "${IDENTITY}" FOUND)
        if(HASH STREQUAL CMAKE_MATCH_1 OR NOT FOUND EQUAL -1)
            set(FOUND 0)
            break()
        endif()
    endforeach()
    if(FOUND EQUAL -1)
        message(FATAL_ERROR "No valid signing identity in the keychain "
            "matches APPLE_CERTIFICATE_ID: ${IDENTITY}")
    endif()
    set(SIGN --force --sign "${IDENTITY}" --timestamp)
    set(EXECUTABLE ${SIGN} --options runtime
        --entitlements "${CMAKE_CURRENT_LIST_DIR}/../entitlements.plist")
else()
    set(SIGN --force --sign -)
    set(EXECUTABLE ${SIGN})
endif()

# after CPack made the packages: the disk images, so that Gatekeeper checks
# them when they're opened
if(CPACK_PACKAGE_FILES)
    if(IDENTITY)
        foreach(FILE IN LISTS CPACK_PACKAGE_FILES)
            if(FILE MATCHES "\\.dmg$")
                codesign(${SIGN} "${FILE}")
            endif()
        endforeach()
    endif()
    return()
endif()

set(DIR "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
if(NOT IS_DIRECTORY "${DIR}")
    message(FATAL_ERROR "macos_codesign.cmake runs from CPack, which stages the files to sign")
endif()
if(IDENTITY)
    message(STATUS "Signing ${DIR} as ${IDENTITY}")
else()
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
    codesign(--verify --strict --deep "${APP}")
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
