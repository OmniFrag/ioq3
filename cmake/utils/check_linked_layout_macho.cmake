# Checks a Mach-O executable for the layout VM_ResetLinked relies on, as
# check_linked_layout.awk does for GNU ld, which Apple's linker has no
# equivalent map for once it links more than one architecture:
# - the module's objects hold no zeroed data but the bss markers, as
#   -fno-zero-initialized-in-bss makes them, since zeroed statics would go
#   to __DATA,__bss, apart from every marker;
# - in each architecture of the executable, the data markers are in __data
#   and the bss markers in zero fill, every writable symbol the module
#   exports (named <module>_...) lies between them, and every symbol between
#   them is one the module's objects define, so the reset clears nothing of
#   the engine's.
# A static can share its name with the engine's or another module's, and
# the linker strips unused ones, so the executable's statics can't be told
# apart by name; the objects are checked for those instead.
#
#   cmake -DNM=<nm> -DBINARY=<executable> -DARCHS=<arch|arch|...>
#         -DMODULES=<module|module|...> -DOBJECTS_<module>=<object|...>
#         -P check_linked_layout_macho.cmake
#
# Prints each stray symbol and fails, or prints nothing.

cmake_minimum_required(VERSION 3.25)

string(REPLACE "|" ";" ARCHS "${ARCHS}")
string(REPLACE "|" ";" MODULES "${MODULES}")
if(NOT ARCHS)
    set(ARCHS "-") # a thin binary: nm without -arch
endif()

set(MARKER "^_([A-Za-z0-9]+)_linked(Data|Bss)(Begin|End)$")
set(WRITABLE "^__DATA,__(data|bss|common)$")
set(ZERO_FILL "^__DATA,__(bss|common)$")

# Runs nm -m, and returns "<address> <section> <name>" for each symbol a
# __DATA or __DATA_CONST section holds, leaving out the assembler's
# temporary labels (ltmp0). nm prints 64-bit addresses in full, so they
# compare in order as strings.
function(data_symbols OUT ARCH)
    set(ARGS -m)
    if(NOT ARCH STREQUAL "-")
        list(PREPEND ARGS -arch ${ARCH})
    endif()
    execute_process(
        COMMAND ${NM} ${ARGS} ${ARGN}
        OUTPUT_VARIABLE NM_OUTPUT
        ERROR_VARIABLE NM_ERROR
        RESULT_VARIABLE NM_RESULT)
    if(NOT NM_RESULT EQUAL 0)
        message(FATAL_ERROR "${NM} failed: ${NM_ERROR}")
    endif()
    string(REGEX MATCHALL "[^\r\n]+" LINES "${NM_OUTPUT}")
    set(SYMBOLS "")
    foreach(LINE IN LISTS LINES)
        # "0000000100257800 (__DATA,__common) non-external ... _name"
        if(LINE MATCHES "^([0-9a-f]+) \\((__DATA[A-Z_]*,[^)]+)\\) .* ([^ l][^ ]*)$")
            list(APPEND SYMBOLS "${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3}")
        endif()
    endforeach()
    set(${OUT} "${SYMBOLS}" PARENT_SCOPE)
endfunction()

set(FAILED FALSE)
foreach(ARCH IN LISTS ARCHS)
    set(WHERE "${BINARY}")
    if(NOT ARCH STREQUAL "-")
        set(WHERE "${BINARY} (${ARCH})")
    endif()
    data_symbols(BINARY_SYMBOLS "${ARCH}" ${BINARY})

    # the markers, found in one pass
    foreach(MODULE IN LISTS MODULES)
        foreach(END_NAME DataBegin DataEnd BssBegin BssEnd)
            unset(AT__${MODULE}_linked${END_NAME})
            unset(SECTION__${MODULE}_linked${END_NAME})
        endforeach()
    endforeach()
    foreach(SYMBOL IN LISTS BINARY_SYMBOLS)
        string(REPLACE " " ";" FIELDS "${SYMBOL}")
        list(GET FIELDS 2 NAME)
        if(NAME MATCHES "${MARKER}")
            list(GET FIELDS 0 AT_${NAME})
            list(GET FIELDS 1 SECTION_${NAME})
        endif()
    endforeach()

    # each module's ranges, between its data markers and its bss markers:
    # the data markers in __data, and the bss markers in zero fill, which
    # the reset clears (a bss range in __data would clear the module's
    # initialized data)
    foreach(MODULE IN LISTS MODULES)
        set(KINDS_${MODULE} "")
        foreach(KIND Data Bss)
            string(TOLOWER ${KIND} KIND_NAME)
            set(BEGIN "${AT__${MODULE}_linked${KIND}Begin}")
            set(END "${AT__${MODULE}_linked${KIND}End}")
            set(BEGIN_SECTION "${SECTION__${MODULE}_linked${KIND}Begin}")
            set(END_SECTION "${SECTION__${MODULE}_linked${KIND}End}")
            if(KIND STREQUAL "Data")
                set(EXPECTED "^__DATA,__data$")
            else()
                set(EXPECTED "${ZERO_FILL}")
            endif()
            if(NOT BEGIN OR NOT END OR NOT END STRGREATER BEGIN)
                message("${WHERE}: ${MODULE}: no ${KIND_NAME} markers, or out of order")
                set(FAILED TRUE)
            elseif(NOT BEGIN_SECTION STREQUAL END_SECTION OR NOT BEGIN_SECTION MATCHES "${EXPECTED}")
                message("${WHERE}: ${MODULE}: the ${KIND_NAME} markers are in ${BEGIN_SECTION} and ${END_SECTION}")
                set(FAILED TRUE)
            else()
                list(APPEND KINDS_${MODULE} ${KIND})
                set(BEGIN_${MODULE}_${KIND} "${BEGIN}")
                set(END_${MODULE}_${KIND} "${END}")
            endif()
        endforeach()
    endforeach()

    # the module each symbol lies in, if any: "<module or -> <section> <name>"
    set(PLACED "")
    foreach(SYMBOL IN LISTS BINARY_SYMBOLS)
        string(REPLACE " " ";" FIELDS "${SYMBOL}")
        list(GET FIELDS 0 AT)
        list(GET FIELDS 1 SECTION)
        list(GET FIELDS 2 NAME)
        if(NAME MATCHES "${MARKER}")
            continue()
        endif()
        set(IN "-")
        foreach(MODULE IN LISTS MODULES)
            foreach(KIND IN LISTS KINDS_${MODULE})
                if(NOT AT STRLESS BEGIN_${MODULE}_${KIND} AND AT STRLESS END_${MODULE}_${KIND})
                    set(IN ${MODULE})
                    break()
                endif()
            endforeach()
            if(NOT IN STREQUAL "-")
                break()
            endif()
        endforeach()
        list(APPEND PLACED "${IN} ${SECTION} ${NAME}")
    endforeach()

    foreach(MODULE IN LISTS MODULES)
        string(REPLACE "|" ";" OBJECTS "${OBJECTS_${MODULE}}")
        data_symbols(OBJECT_SYMBOLS "${ARCH}" ${OBJECTS})

        # the names the module's objects define, and any zeroed data
        set(OWN "")
        foreach(SYMBOL IN LISTS OBJECT_SYMBOLS)
            string(REPLACE " " ";" FIELDS "${SYMBOL}")
            list(GET FIELDS 1 SECTION)
            list(GET FIELDS 2 NAME)
            if(NAME MATCHES "${MARKER}")
                continue()
            endif()
            list(APPEND OWN "${NAME}")
            if(SECTION MATCHES "${ZERO_FILL}")
                message("${WHERE}: ${MODULE}: ${NAME} is in ${SECTION}, outside the module's markers, where loading the module won't reset it")
                set(FAILED TRUE)
            endif()
        endforeach()
        if(NOT KINDS_${MODULE})
            continue()
        endif()

        foreach(ENTRY IN LISTS PLACED)
            string(REPLACE " " ";" FIELDS "${ENTRY}")
            list(GET FIELDS 0 IN)
            list(GET FIELDS 1 SECTION)
            list(GET FIELDS 2 NAME)
            if(IN STREQUAL MODULE AND NOT NAME IN_LIST OWN)
                message("${WHERE}: ${MODULE}: ${NAME} lies between the module's markers")
                set(FAILED TRUE)
            elseif(NOT IN STREQUAL MODULE AND NAME MATCHES "^_${MODULE}_" AND SECTION MATCHES "${WRITABLE}")
                message("${WHERE}: ${MODULE}: ${NAME} lies outside the module's markers, where loading the module won't reset it")
                set(FAILED TRUE)
            endif()
        endforeach()
    endforeach()
endforeach()

if(FAILED)
    message(FATAL_ERROR "VM_ResetLinked can't reset the linked modules of ${BINARY} safely")
endif()
