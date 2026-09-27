# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FREECAD_WASM only (see cMake/FreeCAD_Helpers/SetupWasm.cmake).
#
#   FreeCADWasm         static library: FreeCADWasm_setupEnvironment() and the
#                       generated FreeCADWasm_registerBuiltinModules()
#   freecad_wasm_smoke  node test: App init + Part::Box + recompute + volume
#   freecad_api         the API module: src/Api's C ABI (fcapi_*) as an ES6
#                       JS+wasm module (BUILD_API=ON)

# --------------------------------------------------------------------------
# built-in Python modules
# --------------------------------------------------------------------------
# src/Mod/<dir>  ->  CMake target ; Python module name (PyInit_<name>)
set(_fc_wasm_map_Material   "Materials;Materials")
set(_fc_wasm_map_Part       "Part;Part")
set(_fc_wasm_map_Sketcher   "Sketcher;Sketcher")
set(_fc_wasm_map_PartDesign "PartDesign;_PartDesign")

set(FC_WASM_DECLS "")
set(FC_WASM_REGS "")
set(FC_WASM_NAMES "")
set(FREECAD_WASM_MODULE_TARGETS "")
foreach(_mod IN LISTS FREECAD_WASM_MODULES)
    if(NOT DEFINED _fc_wasm_map_${_mod})
        message(FATAL_ERROR "FREECAD_WASM: no built-in module mapping for '${_mod}' "
                            "(add one to src/Wasm/CMakeLists.txt)")
    endif()
    list(GET _fc_wasm_map_${_mod} 0 _target)
    list(GET _fc_wasm_map_${_mod} 1 _pyname)
    if(NOT TARGET ${_target})
        message(FATAL_ERROR "FREECAD_WASM: module '${_mod}' has no target '${_target}' - is BUILD_* ON?")
    endif()
    list(APPEND FREECAD_WASM_MODULE_TARGETS ${_target})
    string(APPEND FC_WASM_DECLS "PyObject* PyInit_${_pyname}(void);\n")
    string(APPEND FC_WASM_REGS "    PyImport_AppendInittab(\"${_pyname}\", PyInit_${_pyname});\n")
    string(APPEND FC_WASM_NAMES "        \"${_pyname}\",\n")
endforeach()
set(FREECAD_WASM_MODULE_TARGETS ${FREECAD_WASM_MODULE_TARGETS} PARENT_SCOPE)

configure_file(FreeCADWasmModules.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/FreeCADWasmModules.cpp @ONLY)

add_library(FreeCADWasm STATIC
    FreeCADWasm.cpp
    FreeCADWasm.h
    ${CMAKE_CURRENT_BINARY_DIR}/FreeCADWasmModules.cpp
)
target_include_directories(FreeCADWasm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${Python3_INCLUDE_DIRS}
)
target_link_libraries(FreeCADWasm PUBLIC ${FREECAD_WASM_MODULE_TARGETS} FreeCADApp FreeCADBase)

# --------------------------------------------------------------------------
# what a final module links and ships
# --------------------------------------------------------------------------
# Libraries the static archives need but do not carry: CPython's own
# dependencies (it was built against the prefix's zlib) and the ICU data stub.
set(FREECAD_WASM_EXTRA_LIBS "" CACHE STRING
    "Extra libraries for the final wasm link (zlib, libicudata, ...); set by tools/wasm/configure.sh")
set(FREECAD_WASM_PYTHON_STDLIB_ZIP "" CACHE FILEPATH
    "Zipped CPython stdlib, preloaded as /freecad/lib/python3XY.zip")
set(FREECAD_WASM_ICU_DATA "" CACHE FILEPATH
    "ICU data archive (icudtNNl.dat), preloaded into /freecad/share/icu")

if(NOT EXISTS "${FREECAD_WASM_PYTHON_STDLIB_ZIP}")
    message(FATAL_ERROR "FREECAD_WASM_PYTHON_STDLIB_ZIP='${FREECAD_WASM_PYTHON_STDLIB_ZIP}' does not exist")
endif()
if(NOT EXISTS "${FREECAD_WASM_ICU_DATA}")
    message(FATAL_ERROR "FREECAD_WASM_ICU_DATA='${FREECAD_WASM_ICU_DATA}' does not exist")
endif()
get_filename_component(_fc_wasm_pyzip_name "${FREECAD_WASM_PYTHON_STDLIB_ZIP}" NAME)
get_filename_component(_fc_wasm_icu_name "${FREECAD_WASM_ICU_DATA}" NAME)

# The MEMFS image, /freecad: FreeCAD's Mod/ and Ext/ trees as they are laid
# out in the build directory (Python sources, material cards, ...), the
# stdlib zip and the ICU data.  Compiled module archives never land there
# (static libraries go to ARCHIVE_OUTPUT_DIRECTORY, i.e. next to their
# sources), but exclude the usual suspects anyway.
set(FREECAD_WASM_PRELOAD_FLAGS
    "SHELL:--preload-file ${CMAKE_BINARY_DIR}/Mod@/freecad/Mod"
    "SHELL:--preload-file ${CMAKE_BINARY_DIR}/Ext@/freecad/Ext"
    "SHELL:--preload-file ${FREECAD_WASM_PYTHON_STDLIB_ZIP}@/freecad/lib/${_fc_wasm_pyzip_name}"
    "SHELL:--preload-file ${FREECAD_WASM_ICU_DATA}@/freecad/share/icu/${_fc_wasm_icu_name}"
    "SHELL:--exclude-file *.a"
    "SHELL:--exclude-file *.so"
    "SHELL:--exclude-file __pycache__"
)

set(FREECAD_WASM_LINK_OPTIONS
    -fwasm-exceptions
    -sWASM_LEGACY_EXCEPTIONS=1
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=128MB
    -sSTACK_SIZE=8MB
    -sFORCE_FILESYSTEM=1
    -sENVIRONMENT=node,web,worker
    ${FREECAD_WASM_PRELOAD_FLAGS}
)
set(FREECAD_WASM_LINK_OPTIONS ${FREECAD_WASM_LINK_OPTIONS} PARENT_SCOPE)

# The file-system image changes whenever a .py file under Mod/ does; relink.
set(_fc_wasm_link_depends ${FREECAD_WASM_PYTHON_STDLIB_ZIP} ${FREECAD_WASM_ICU_DATA})

# --------------------------------------------------------------------------
# freecad_wasm_smoke
# --------------------------------------------------------------------------
add_executable(freecad_wasm_smoke freecad_wasm_smoke.cpp)
target_include_directories(freecad_wasm_smoke PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(freecad_wasm_smoke PRIVATE FreeCADWasm ${FREECAD_WASM_EXTRA_LIBS})
target_link_options(freecad_wasm_smoke PRIVATE ${FREECAD_WASM_LINK_OPTIONS})
set_target_properties(freecad_wasm_smoke PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/wasm
    LINK_DEPENDS "${_fc_wasm_link_depends}"
)
# The Mod/ and Ext/ trees are filled by the modules' copy targets (custom
# "ALL" targets: PartScripts, MaterialLib, ...); the image must not be packed
# before they ran.
function(fc_wasm_collect_utility_targets dir out)
    set(_result)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_property(_type TARGET ${_t} PROPERTY TYPE)
        if(_type STREQUAL "UTILITY")
            list(APPEND _result ${_t})
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        fc_wasm_collect_utility_targets("${_sub}" _subresult)
        list(APPEND _result ${_subresult})
    endforeach()
    set(${out} ${_result} PARENT_SCOPE)
endfunction()

set(FREECAD_WASM_DATA_TARGETS "")
foreach(_dir ${CMAKE_SOURCE_DIR}/src/Ext ${CMAKE_SOURCE_DIR}/src/Mod)
    fc_wasm_collect_utility_targets(${_dir} _found)
    list(APPEND FREECAD_WASM_DATA_TARGETS ${_found})
endforeach()
set(FREECAD_WASM_DATA_TARGETS ${FREECAD_WASM_DATA_TARGETS} PARENT_SCOPE)
if(FREECAD_WASM_DATA_TARGETS)
    add_dependencies(freecad_wasm_smoke ${FREECAD_WASM_DATA_TARGETS})
endif()

# --------------------------------------------------------------------------
# freecad_api: freecad_api.js + .wasm + .data
# --------------------------------------------------------------------------
if(TARGET FreeCADApiCore)
    add_executable(freecad_api freecad_api_main.cpp)
    # FreeCADApiCore whole: its command handlers must all be in the image,
    # not only the ones the exported entry points happen to reference.
    target_link_libraries(freecad_api PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,FreeCADApiCore>"
        FreeCADWasm
        ${FREECAD_WASM_EXTRA_LIBS}
    )
    target_link_options(freecad_api PRIVATE
        ${FREECAD_WASM_LINK_OPTIONS}
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createFreecadApi
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_fcapi_init,_fcapi_dispatch,_fcapi_free,_fcapi_shutdown,_fcapi_last_error"
        "-sEXPORTED_RUNTIME_METHODS=HEAPU8,FS,UTF8ToString,stringToNewUTF8,ccall,cwrap"
    )
    set_target_properties(freecad_api PROPERTIES
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/wasm
        LINK_DEPENDS "${_fc_wasm_link_depends}"
    )
    if(FREECAD_WASM_DATA_TARGETS)
        add_dependencies(freecad_api ${FREECAD_WASM_DATA_TARGETS})
    endif()
endif()
