# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(Build)
add_subdirectory(3rdParty)
add_subdirectory(Base)
add_subdirectory(App)
if(BUILD_API)
    add_subdirectory(Api)
endif(BUILD_API)
if(NOT FREECAD_WASM)
    # FreeCADMain / FreeCADCmd: executables and a loadable Python module.
    # The wasm build has its own entry points in src/Wasm.
    add_subdirectory(Main)
endif()
add_subdirectory(Mod)
add_subdirectory(Ext)
add_subdirectory(Doc)

if(BUILD_GUI)
    add_subdirectory(Gui)
    if(UNIX AND NOT APPLE)
        add_subdirectory(XDGData)
    endif()
endif(BUILD_GUI)

if(BUILD_TEMPLATE)
    add_subdirectory(Tools/_TEMPLATE_)
endif(BUILD_TEMPLATE)

if(FREECAD_WASM)
    # After Mod/ and Ext/: it links their targets and packs their files.
    add_subdirectory(Wasm)
endif()

# "if" clause moved into local CMakeLists.txt file to support Conda and Homebrew builds
add_subdirectory(MacAppBundle)

