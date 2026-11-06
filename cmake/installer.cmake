set(INSTALL_TARGETS)
foreach(DIRECTORY IN ITEMS ${PROJECT_SOURCE_DIR} ${EXTRA_GAME_DIRECTORIES})
    get_directory_property(DIRECTORY_TARGETS DIRECTORY
        ${DIRECTORY} BUILDSYSTEM_TARGETS)
    list(APPEND INSTALL_TARGETS ${DIRECTORY_TARGETS})
endforeach()

# Iterate over all the targets that have an INSTALL_DESTINATION
# property (set by set_output_dirs) and call install() on them, with the
# files they need beside them (INSTALL_FILES), in their INSTALL_COMPONENT
# if set, so a project can install parts of its build separately
foreach(TARGET IN LISTS INSTALL_TARGETS)
    get_target_property(DESTINATION ${TARGET} INSTALL_DESTINATION)
    if(NOT DESTINATION)
        continue()
    endif()

    get_target_property(COMPONENT ${TARGET} INSTALL_COMPONENT)
    set(COMPONENT_ARGS)
    if(COMPONENT)
        set(COMPONENT_ARGS COMPONENT ${COMPONENT})
    endif()

    install(TARGETS ${TARGET}
        RUNTIME DESTINATION ${DESTINATION} ${COMPONENT_ARGS}
        LIBRARY DESTINATION ${DESTINATION} ${COMPONENT_ARGS}
        BUNDLE DESTINATION ${DESTINATION} ${COMPONENT_ARGS}
        ARCHIVE EXCLUDE_FROM_ALL)

    get_target_property(FILES ${TARGET} INSTALL_FILES)
    if(FILES)
        install(FILES ${FILES} DESTINATION
            # install() requires a relative path hence:
            $<PATH:RELATIVE_PATH,$<TARGET_FILE_DIR:${TARGET}>,${CMAKE_BINARY_DIR}/$<CONFIG>>
            ${COMPONENT_ARGS})
    endif()
endforeach()

if(NOT CPACK_GENERATOR)
    return()
endif()

set(CPACK_PACKAGE_NAME ${PACKAGE_NAME})
set(CPACK_PACKAGE_INSTALL_DIRECTORY ${PACKAGE_NAME})
set(CPACK_PACKAGE_EXECUTABLES ${CLIENT_NAME} ${PACKAGE_NAME})
set(CPACK_PACKAGE_FILE_NAME ${PACKAGE_NAME}-${PRODUCT_VERSION})
set(CPACK_PACKAGE_CONTACT ${CONTACT_EMAIL})
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/COPYING.txt)

include(CPack)
