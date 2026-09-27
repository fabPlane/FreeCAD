# SPDX-License-Identifier: LGPL-2.1-or-later

# The FreeCAD API (see PROTOCOL.md).
#
#  FreeCADApiCore  the dispatcher, the command handlers and the transports (a library)
#  FreeCADApi      Python module: start the server inside a running FreeCAD
#
# FreeCADApiServer, the stand-alone server, is built in src/Main.

find_package(Threads REQUIRED)

set(FreeCADApiCore_SRCS
    ApiC.cpp
    ApiC.h
    ApiGlobal.h
    Codec.cpp
    Codec.h
    Executor.cpp
    Executor.h
    HandlersDocument.cpp
    HandlersGeometry.cpp
    HandlersObject.cpp
    HandlersScript.cpp
    HandlersServer.cpp
    Host.cpp
    Host.h
    Paths.h
    Server.cpp
    Server.h
    Transaction.h
    Values.cpp
    Values.h
)

# There are no sockets or pipes in WebAssembly; the module is reached through the C ABI.
if(NOT EMSCRIPTEN)
    list(APPEND FreeCADApiCore_SRCS
        StdioTransport.cpp
        StdioTransport.h
        WebSocketTransport.cpp
        WebSocketTransport.h
    )
endif()

if(EMSCRIPTEN)
    add_library(FreeCADApiCore STATIC ${FreeCADApiCore_SRCS})
else()
    add_library(FreeCADApiCore SHARED ${FreeCADApiCore_SRCS})
endif()

target_include_directories(
    FreeCADApiCore
    PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(
    FreeCADApiCore
    SYSTEM
    PUBLIC
    ${nlohmann_json_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${Python3_INCLUDE_DIRS}
    ${QtCore_INCLUDE_DIRS}
)

target_link_libraries(FreeCADApiCore
    FreeCADApp
    ${QtCore_LIBRARIES}
    Threads::Threads
)
if(WIN32)
    target_link_libraries(FreeCADApiCore ws2_32 mswsock)
endif()

if(MSVC)
    target_compile_options(FreeCADApiCore PRIVATE /wd4251 /wd4275 /bigobj)
endif()

target_compile_warn_error(FreeCADApiCore)
SET_BIN_DIR(FreeCADApiCore FreeCADApiCore)
INSTALL(TARGETS FreeCADApiCore
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(NOT EMSCRIPTEN)
    add_library(FreeCADApi SHARED AppApiPy.cpp)
    target_include_directories(FreeCADApi PRIVATE ${CMAKE_BINARY_DIR}/src ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(FreeCADApi FreeCADApiCore)
    if(NOT BUILD_DYNAMIC_LINK_PYTHON)
        target_link_libraries(FreeCADApi ${Python3_LIBRARIES})
    endif()
    SET_BIN_DIR(FreeCADApi FreeCADApi)
    SET_PYTHON_PREFIX_SUFFIX(FreeCADApi)
    INSTALL(TARGETS FreeCADApi
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
