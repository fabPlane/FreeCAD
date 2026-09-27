# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FREECAD_WASM: build FreeCAD's application core (no GUI) for WebAssembly
# with Emscripten, as ONE statically linked module.
#
# Natively every FreeCAD module (Part, Sketcher, ...) is a shared library that
# Python loads when something runs `import Part`.  Emscripten has no dlopen we
# want to rely on, so with FREECAD_WASM=ON:
#
#   * every add_library(... SHARED|MODULE) in the tree becomes a STATIC
#     library (the global TARGET_SUPPORTS_SHARED_LIBS property - CMake then
#     builds a static archive for them, with a one-line warning per target);
#   * only the modules in FREECAD_WASM_MODULES are built - every other
#     BUILD_<MODULE> option is forced OFF, and BUILD_GUI with them;
#   * src/Wasm links the libraries into the final wasm targets and generates
#     FreeCADWasmModules.cpp, whose FreeCADWasm_registerBuiltinModules()
#     calls PyImport_AppendInittab() for each module's PyInit_<name>, so that
#     `import Part` finds a BUILT-IN module.  It has to run before
#     Py_Initialize(), i.e. before App::Application::init().
#
# SetupWasm() runs BEFORE InitializeFreeCADBuildOptions(): the forced cache
# values then win over the option() defaults, and everything derived from
# them there (FREECAD_USE_SMESH, FREECAD_USE_PYBIND11, ...) comes out right.
#
# With FREECAD_WASM=OFF (the default) nothing in this file has any effect.

macro(SetupWasm)
    option(FREECAD_WASM
        "Build the application core (no GUI) for WebAssembly: static modules registered as Python built-ins"
        OFF)

    if(FREECAD_WASM)
        if(NOT EMSCRIPTEN)
            message(FATAL_ERROR "FREECAD_WASM=ON needs the Emscripten toolchain (configure with emcmake, see tools/wasm/)")
        endif()

        # The modules that go into the wasm module, in FreeCAD's directory
        # names (src/Mod/<name>).  Their BUILD_* options stay as given; all
        # others are turned off.
        set(FREECAD_WASM_MODULES "Material;Part;Sketcher;PartDesign" CACHE STRING
            "src/Mod directories linked into the WebAssembly module")

        # add_library(SHARED|MODULE) -> STATIC everywhere
        set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
        set(BUILD_SHARED_LIBS OFF)

        set(_fc_wasm_all_mods
            ADDONMGR ASSEMBLY BIM CAM DRAFT FEM FLAT_MESH HELP IMPORT INSPECTION JTREADER
            MATERIAL MATERIAL_EXTERNAL MEASURE MESH MESH_PART OPENSCAD PART PART_DESIGN
            PLOT POINTS REVERSEENGINEERING ROBOT SHOW SKETCHER SPREADSHEET START SURFACE
            TECHDRAW TEMPLATE TEST TUX WEB)
        set(_fc_wasm_keep)
        foreach(_mod IN LISTS FREECAD_WASM_MODULES)
            if(_mod STREQUAL "PartDesign")
                list(APPEND _fc_wasm_keep PART_DESIGN)
            else()
                string(TOUPPER "${_mod}" _umod)
                list(APPEND _fc_wasm_keep ${_umod})
            endif()
        endforeach()
        foreach(_mod IN LISTS _fc_wasm_all_mods)
            if(NOT _mod IN_LIST _fc_wasm_keep)
                set(BUILD_${_mod} OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)
            endif()
        endforeach()

        set(BUILD_GUI OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)
        set(ENABLE_DEVELOPER_TESTS OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)
        set(BUILD_DESIGNER_PLUGIN OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)
        set(BUILD_FEM_NETGEN OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)
        set(FREECAD_USE_PCL OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)
        set(FREECAD_USE_3DCONNEXION_LEGACY OFF CACHE BOOL "Forced OFF by FREECAD_WASM" FORCE)

        # Boost.Thread (boost::mutex: Part's and Sketcher's random-tag
        # generators) needs BOOST_HAS_PTHREADS, which boost/config derives from
        # _POSIX_THREADS - not defined without -pthread.  Emscripten's pthread
        # functions exist anyway (single-threaded: a mutex always succeeds),
        # which is all these uses need.
        add_compile_definitions(BOOST_HAS_PTHREADS)

        message(STATUS "FREECAD_WASM: static WebAssembly build of ${FREECAD_WASM_MODULES}")
    endif()
endmacro(SetupWasm)
