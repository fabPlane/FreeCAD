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

// Entry point of the freecad_api WebAssembly module.
//
// The C ABI itself (fcapi_init / fcapi_dispatch / ...) is src/Api/ApiC.cpp.
// main() runs once when the module is instantiated, before JavaScript can
// call fcapi_init(), and prepares what App::Application::init() - called by
// fcapi_init() through Api::initializeFreeCAD() - needs in a browser: the
// MEMFS environment and the statically linked FreeCAD modules registered as
// built-in Python modules (PyImport_AppendInittab only works before
// Py_Initialize).  The runtime stays alive after main returns
// (EXIT_RUNTIME=0, the default).

#include "FreeCADWasm.h"

int main()
{
    FreeCADWasm_setupEnvironment();
    FreeCADWasm_registerBuiltinModules();
    return 0;
}
