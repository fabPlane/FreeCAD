// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>

#include "ApiGlobal.h"

namespace Api
{

/**
 * Start FreeCAD's application core without a GUI, the way FreeCADCmd does, for a process whose
 * only job is to serve the API (FreeCADApiServer, the wasm module). Does nothing when the
 * application is already running (the API was loaded into a FreeCAD). Returns false and fills
 * `error` when initialisation fails.
 */
ApiExport bool initializeFreeCAD(int argc, char** argv, std::string& error);

/// True once initializeFreeCAD() succeeded in this process.
ApiExport bool freeCADInitialized();

/// Close every document and tear the application down (only if initializeFreeCAD() started it).
ApiExport void shutdownFreeCAD();

}  // namespace Api
