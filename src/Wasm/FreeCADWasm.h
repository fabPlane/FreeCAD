// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD project association AISBL               *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

// Glue for the FREECAD_WASM build (see cMake/FreeCAD_Helpers/SetupWasm.cmake
// and tools/wasm/README.md).  Anything that hosts FreeCAD's application core
// in a WebAssembly module calls, in this order and before
// App::Application::init():
//
//     FreeCADWasm_setupEnvironment();        // MEMFS paths, locale, ICU
//     FreeCADWasm_registerBuiltinModules();  // PyImport_AppendInittab(...)

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Register every module linked into this image (FREECAD_WASM_MODULES) as a
/// built-in Python module, so that `import Part` needs no shared library.
/// Must run before Py_Initialize().  Generated: FreeCADWasmModules.cpp.
void FreeCADWasm_registerBuiltinModules(void);

/// The names registered by FreeCADWasm_registerBuiltinModules(),
/// NULL-terminated.
const char* const* FreeCADWasm_builtinModuleNames(void);

/// Default environment for a module whose data is mounted at /freecad:
/// FREECAD_HOME, PYTHONHOME, FREECAD_USER_HOME, ICU_DATA (each only if not
/// already set) and a UTF-8 C locale.
void FreeCADWasm_setupEnvironment(void);

#ifdef __cplusplus
}
#endif
