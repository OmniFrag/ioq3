if(NOT BUILD_CLIENT)
    return()
endif()

include(utils/add_git_dependency)
include(utils/set_output_dirs)
include(shared_sources)

include(renderer_common)

set(CLIENT_SOURCES
    ${SOURCE_DIR}/client/cl_cgame.c
    ${SOURCE_DIR}/client/cl_cin.c
    ${SOURCE_DIR}/client/cl_console.c
    ${SOURCE_DIR}/client/cl_content.c
    ${SOURCE_DIR}/client/cl_demoinfo.c
    ${SOURCE_DIR}/client/cl_ext.c
    ${SOURCE_DIR}/client/cl_input.c
    ${SOURCE_DIR}/client/cl_keys.c
    ${SOURCE_DIR}/client/cl_main.c
    ${SOURCE_DIR}/client/cl_net_chan.c
    ${SOURCE_DIR}/client/cl_parse.c
    ${SOURCE_DIR}/client/cl_present.c
    ${SOURCE_DIR}/client/cl_scrn.c
    ${SOURCE_DIR}/client/cl_ui.c
    ${SOURCE_DIR}/client/cl_uispeeds.c
    ${SOURCE_DIR}/client/cl_uispeeds_stats.c
    ${SOURCE_DIR}/client/cl_avi.c
    ${SOURCE_DIR}/client/libmumblelink.c
    ${SOURCE_DIR}/client/snd_altivec.c
    ${SOURCE_DIR}/client/snd_adpcm.c
    ${SOURCE_DIR}/client/snd_dma.c
    ${SOURCE_DIR}/client/snd_mem.c
    ${SOURCE_DIR}/client/snd_mix.c
    ${SOURCE_DIR}/client/snd_wavelet.c
    ${SOURCE_DIR}/client/snd_main.c
    ${SOURCE_DIR}/client/snd_codec.c
    ${SOURCE_DIR}/client/snd_codec_wav.c
    ${SOURCE_DIR}/client/snd_codec_ogg.c
    ${SOURCE_DIR}/client/snd_codec_opus.c
    ${SOURCE_DIR}/client/qal.c
    ${SOURCE_DIR}/client/snd_openal.c
    ${SOURCE_DIR}/sdl/sdl_input.c
    ${SOURCE_DIR}/sdl/sdl_snd.c
    ${CLIENT_PLATFORM_SOURCES}
)

if(USE_WEB_AUDIO)
    list(REMOVE_ITEM CLIENT_SOURCES ${SOURCE_DIR}/sdl/sdl_snd.c)
    list(APPEND CLIENT_SOURCES ${SOURCE_DIR}/web/snd_web.c)
    list(APPEND CLIENT_DEFINITIONS USE_WEB_AUDIO)
endif()

# the browser's connections to servers (NA_WEB), with net_web.js
if(EMSCRIPTEN)
    list(APPEND CLIENT_SOURCES ${SOURCE_DIR}/web/net_web.c)
endif()

add_git_dependency(${SOURCE_DIR}/client/cl_console.c)

set(CLIENT_BINARY ${CLIENT_NAME})

list(APPEND CLIENT_DEFINITIONS BOTLIB)

if(BUILD_STANDALONE)
    list(APPEND CLIENT_DEFINITIONS STANDALONE)
endif()

if(USE_RENDERER_DLOPEN)
    list(APPEND CLIENT_DEFINITIONS USE_RENDERER_DLOPEN)
endif()

if(USE_HTTP)
    list(APPEND CLIENT_DEFINITIONS USE_HTTP)
endif()

if(USE_VOIP)
    list(APPEND CLIENT_DEFINITIONS USE_VOIP)
endif()

if(USE_MUMBLE)
    list(APPEND CLIENT_DEFINITIONS USE_MUMBLE)
    list(APPEND CLIENT_LIBRARY_SOURCES ${SOURCE_DIR}/client/libmumblelink.c)
endif()

list(APPEND CLIENT_BINARY_SOURCES
    ${SERVER_SOURCES}
    ${CLIENT_SOURCES}
    ${COMMON_SOURCES}
    ${BOTLIB_SOURCES}
    ${SYSTEM_SOURCES}
    ${ASM_SOURCES}
    ${CLIENT_ASM_SOURCES}
    ${CLIENT_LIBRARY_SOURCES})

add_executable(${CLIENT_BINARY} ${CLIENT_EXECUTABLE_OPTIONS} ${CLIENT_BINARY_SOURCES})

target_include_directories(     ${CLIENT_BINARY} PRIVATE ${CLIENT_INCLUDE_DIRS})
target_compile_definitions(     ${CLIENT_BINARY} PRIVATE ${CLIENT_DEFINITIONS})
target_compile_options(         ${CLIENT_BINARY} PRIVATE ${CLIENT_COMPILE_OPTIONS})
target_link_libraries(          ${CLIENT_BINARY} PRIVATE ${COMMON_LIBRARIES} ${CLIENT_LIBRARIES})
target_link_options(            ${CLIENT_BINARY} PRIVATE ${CLIENT_LINK_OPTIONS})

set_output_dirs(${CLIENT_BINARY})

if(EMSCRIPTEN)
    # relinked when the page's half of the network changes
    set_property(TARGET ${CLIENT_BINARY} APPEND PROPERTY LINK_DEPENDS ${SOURCE_DIR}/web/net_web.js)
endif()

if(NOT USE_RENDERER_DLOPEN)
    target_sources(${CLIENT_BINARY} PRIVATE
        # These are never simultaneously populated
        ${RENDERER_GL1_BINARY_SOURCES}
        ${RENDERER_GL2_BINARY_SOURCES})

    target_include_directories( ${CLIENT_BINARY} PRIVATE ${RENDERER_INCLUDE_DIRS})
    target_compile_definitions( ${CLIENT_BINARY} PRIVATE ${RENDERER_DEFINITIONS})
    target_compile_options(     ${CLIENT_BINARY} PRIVATE ${RENDERER_COMPILE_OPTIONS})
    target_link_libraries(      ${CLIENT_BINARY} PRIVATE ${RENDERER_LIBRARIES})
endif()

foreach(LIBRARY IN LISTS CLIENT_DEPLOY_LIBRARIES)
    add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            ${LIBRARY}
            $<TARGET_FILE_DIR:${CLIENT_BINARY}>)

    # installed beside the client (installer.cmake)
    set_property(TARGET ${CLIENT_BINARY} APPEND PROPERTY INSTALL_FILES ${LIBRARY})
endforeach()

# BUNDLE_SDL: the system SDL3 beside the client, under the soname it asks
# for, and looked for there first: $ORIGIN alone, as a build RPATH would
# leave empty entries, which the loader reads as the current directory.
# sdl.cmake turns BUNDLE_SDL off where it doesn't apply
if(BUNDLE_SDL)
    set(BUNDLED_SDL $<TARGET_FILE_DIR:${CLIENT_BINARY}>/$<TARGET_SONAME_FILE_NAME:SDL3::SDL3-shared>)
    add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:SDL3::SDL3-shared> ${BUNDLED_SDL})
    set_property(TARGET ${CLIENT_BINARY} APPEND PROPERTY INSTALL_FILES ${BUNDLED_SDL})

    foreach(BINARY IN ITEMS ${CLIENT_BINARY} ${RENDERER_GL1_BINARY} ${RENDERER_GL2_BINARY})
        if(TARGET ${BINARY})
            # ahead of any install RPATH already set (CMAKE_INSTALL_RPATH)
            get_target_property(RPATH ${BINARY} INSTALL_RPATH)
            if(NOT RPATH)
                set(RPATH)
            endif()
            list(PREPEND RPATH "\$ORIGIN")
            list(REMOVE_DUPLICATES RPATH)
            set_target_properties(${BINARY} PROPERTIES
                INSTALL_RPATH "${RPATH}"
                BUILD_WITH_INSTALL_RPATH ON)
        endif()
    endforeach()
endif()
