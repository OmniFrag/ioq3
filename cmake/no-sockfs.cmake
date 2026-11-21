# The web client links no socket emulation (code/web/no_sockets.h): the
# build fails if Emscripten's SOCKFS, which connects over WebSockets, is in
# the page's script, as it is once anything calls a socket function. The
# script is removed, so the next build links and checks it again.
#
#   cmake -DSCRIPT=<the client's .js> -P no-sockfs.cmake

file(READ ${SCRIPT} TEXT)
string(FIND "${TEXT}" "SOCKFS" AT)
if(NOT AT EQUAL -1)
    file(REMOVE ${SCRIPT})
    message(FATAL_ERROR "${SCRIPT} has Emscripten's socket emulation (SOCKFS): something on the web calls a "
        "socket function that code/web/no_sockets.h doesn't stand in for")
endif()
