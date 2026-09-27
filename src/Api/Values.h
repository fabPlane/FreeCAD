// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Python.h>

#include "Codec.h"

namespace App
{
class Document;
class DocumentObject;
class Property;
}  // namespace App

namespace Api
{

/**
 * Python <-> JSON with the tagged forms of PROTOCOL.md ("Values"). Property values go through
 * Python because every FreeCAD property can already convert itself to and from a Python
 * object; that makes the API cover every property type, including ones added by modules,
 * without a converter per C++ class. Both functions need the GIL.
 */
Json pyToJson(PyObject* object, int depth = 0);

/// New reference. `doc` resolves {"$type":"Object"} without a "doc" key. Throws ApiError.
PyObject* jsonToPy(const Json& value, App::Document* doc);

/// PropertyInfo of PROTOCOL.md.
Json propertyInfo(const App::DocumentObject* object, App::Property* prop, bool withValue = true);

/// Set a property from a tagged JSON value. Throws ApiError / Base::Exception.
void setPropertyValue(App::DocumentObject* object, App::Property* prop, const Json& value);

Json objectRef(const App::DocumentObject* object);

}  // namespace Api
