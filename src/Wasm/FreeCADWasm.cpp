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

#include "FreeCADWasm.h"

#include <clocale>
#include <cstdlib>
#include <sys/stat.h>

void FreeCADWasm_setupEnvironment(void)
{
    // Everything the module ships is preloaded under /freecad (see
    // src/Wasm/CMakeLists.txt): Mod/, Ext/, lib/python3XY.zip, share/icu/.
    setenv("FREECAD_HOME", "/freecad", 0);
    setenv("PYTHONHOME", "/freecad", 0);
    // ICU is built with --with-data-packaging=archive: the data is a file.
    setenv("ICU_DATA", "/freecad/share/icu", 0);
    // User parameters, macros, caches: a writable MEMFS directory.
    setenv("FREECAD_USER_HOME", "/home/web_user", 0);
    setenv("HOME", "/home/web_user", 0);
    mkdir("/home", 0755);
    mkdir("/home/web_user", 0755);
    mkdir("/tmp", 01777);
    setenv("TMPDIR", "/tmp", 0);

    // musl starts in the "C" locale, in which mbstowcs/wctomb reject every
    // non-ASCII byte.  FreeCAD's MainCmd does setlocale(LC_ALL, "") and then
    // LC_NUMERIC "C"; there is no environment locale here, so say C.UTF-8.
    setlocale(LC_ALL, "C.UTF-8");
    setlocale(LC_NUMERIC, "C");
}
