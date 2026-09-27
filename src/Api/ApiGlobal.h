// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>

#ifndef ApiExport
# ifdef FreeCADApiCore_EXPORTS
#  define ApiExport FREECAD_DECL_EXPORT
# else
#  define ApiExport FREECAD_DECL_IMPORT
# endif
#endif

namespace Api
{

/// Protocol version reported by GetVersion. Bump it when a command changes incompatibly.
constexpr int ProtocolVersion = 1;

}  // namespace Api
