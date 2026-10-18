include_guard(GLOBAL)

# add_engine_extension(<name> [CLIENT] [SERVER]
#     [INCLUDE_DIRECTORIES <directory>...]
#     [LIBRARIES <library>...]
#     SOURCES <source>...)
#
# Links an engine extension (code/qcommon/hooks.h) into the client, the
# dedicated server or both. Its sources define one engineExtension_t named
# <name>_extension, which the engine calls at fixed points; the executable's
# engineExtensions table lists it (engine_extensions.cmake). Call it from a
# directory in EXTRA_ENGINE_DIRECTORIES, so the table is generated after it.
#
# The sources are compiled once for each executable, with its definitions,
# include directories and options (DEDICATED for the server, say), and find
# hooks.h and qcommon.h by name. LIBRARIES are linked into each executable
# it goes in.
function(add_engine_extension NAME)
    cmake_parse_arguments(ARG "CLIENT;SERVER" "" "INCLUDE_DIRECTORIES;LIBRARIES;SOURCES" ${ARGN})

    string(MAKE_C_IDENTIFIER "${NAME}" ID)
    if(NOT ID STREQUAL NAME)
        message(FATAL_ERROR "An engine extension's name is a C identifier, which ${NAME} isn't")
    endif()
    if(NOT ARG_CLIENT AND NOT ARG_SERVER)
        message(FATAL_ERROR "Engine extension ${NAME} goes in neither the client nor the server: add CLIENT, SERVER or both")
    endif()

    set(EXECUTABLES "")
    if(ARG_CLIENT AND TARGET ${CLIENT_BINARY})
        list(APPEND EXECUTABLES ${CLIENT_BINARY})
    endif()
    if(ARG_SERVER AND TARGET ${SERVER_BINARY})
        list(APPEND EXECUTABLES ${SERVER_BINARY})
    endif()

    foreach(EXECUTABLE IN LISTS EXECUTABLES)
        # a second extension of the same name fails here, as a target that exists
        set(TARGET ${NAME}_${EXECUTABLE}_extension)
        add_library(${TARGET} OBJECT ${ARG_SOURCES})
        target_compile_definitions(${TARGET} PRIVATE $<TARGET_PROPERTY:${EXECUTABLE},COMPILE_DEFINITIONS>)
        target_include_directories(${TARGET} PRIVATE ${ARG_INCLUDE_DIRECTORIES} ${SOURCE_DIR}/qcommon
            $<TARGET_PROPERTY:${EXECUTABLE},INCLUDE_DIRECTORIES>)
        target_compile_options(${TARGET} PRIVATE $<TARGET_PROPERTY:${EXECUTABLE},COMPILE_OPTIONS>)
        target_link_libraries(${EXECUTABLE} PRIVATE ${TARGET} ${ARG_LIBRARIES})

        set_property(GLOBAL APPEND PROPERTY ENGINE_EXTENSIONS_${EXECUTABLE} ${NAME})
    endforeach()
endfunction()
