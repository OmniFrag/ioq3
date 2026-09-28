include_guard(GLOBAL)

# The width and height of the PNG at PATH, from its header: the signature,
# the IHDR chunk's length (13) and type, then the width and height, bytes 16
# to 23
function(png_size PATH WIDTH_VAR HEIGHT_VAR)
    file(READ ${PATH} HEADER LIMIT 24 HEX)
    string(LENGTH "${HEADER}" LENGTH)
    if(NOT LENGTH EQUAL 48 OR NOT HEADER MATCHES "^89504e470d0a1a0a0000000d49484452")
        message(FATAL_ERROR "${PATH} is not a PNG")
    endif()
    string(SUBSTRING ${HEADER} 32 8 WIDTH)
    string(SUBSTRING ${HEADER} 40 8 HEIGHT)
    math(EXPR WIDTH 0x${WIDTH})
    math(EXPR HEIGHT 0x${HEIGHT})
    set(${WIDTH_VAR} ${WIDTH} PARENT_SCOPE)
    set(${HEIGHT_VAR} ${HEIGHT} PARENT_SCOPE)
endfunction()
