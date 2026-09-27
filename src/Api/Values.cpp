// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Values.h"

#include <cmath>
#include <limits>

#include <Base/Interpreter.h>
#include <Base/Matrix.h>
#include <Base/MatrixPy.h>
#include <Base/Placement.h>
#include <Base/PlacementPy.h>
#include <Base/Quantity.h>
#include <Base/QuantityPy.h>
#include <Base/Rotation.h>
#include <Base/RotationPy.h>
#include <Base/Unit.h>
#include <Base/Vector3D.h>
#include <Base/VectorPy.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectPy.h>
#include <App/Expression.h>
#include <App/ObjectIdentifier.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Server.h"

namespace Api
{

namespace
{

constexpr int maxDepth = 32;

std::string pyStr(PyObject* object, bool repr)
{
    PyObject* text = repr ? PyObject_Repr(object) : PyObject_Str(object);
    if (!text) {
        PyErr_Clear();
        return "<unprintable>";
    }
    const char* utf8 = PyUnicode_AsUTF8(text);
    std::string out = utf8 ? utf8 : "";
    if (!utf8) {
        PyErr_Clear();
    }
    Py_DECREF(text);
    return out;
}

Json vec3(const Base::Vector3d& v)
{
    return Json::array({v.x, v.y, v.z});
}

Json rotationJson(const Base::Rotation& rot)
{
    double q0 = 0;
    double q1 = 0;
    double q2 = 0;
    double q3 = 1;
    rot.getValue(q0, q1, q2, q3);
    Base::Vector3d axis;
    double angle = 0;
    rot.getRawValue(axis, angle);
    return {
        {"$type", "Rotation"},
        {"q", Json::array({q0, q1, q2, q3})},
        {"axis", vec3(axis)},
        {"angle", angle * 180.0 / M_PI}
    };
}

Base::Vector3d vecFrom(const Json& value)
{
    if (value.is_array() && value.size() == 3) {
        return Base::Vector3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    }
    if (value.is_object()) {
        return Base::Vector3d(value.value("x", 0.0), value.value("y", 0.0), value.value("z", 0.0));
    }
    throw ApiError(Status::BadRequest, "A vector is [x, y, z] or {x, y, z}");
}

Base::Rotation rotationFrom(const Json& value)
{
    // [x, y, z, w] or {"q": [...]} or {"axis": [...], "angle": degrees}
    const Json* q = nullptr;
    if (value.is_array()) {
        q = &value;
    }
    else if (value.is_object() && value.contains("q")) {
        q = &value["q"];
    }
    if (q) {
        if (!q->is_array() || q->size() != 4) {
            throw ApiError(Status::BadRequest, "A rotation quaternion is [x, y, z, w]");
        }
        return Base::Rotation(
            (*q)[0].get<double>(),
            (*q)[1].get<double>(),
            (*q)[2].get<double>(),
            (*q)[3].get<double>()
        );
    }
    if (value.is_object() && value.contains("axis")) {
        return Base::Rotation(vecFrom(value["axis"]), value.value("angle", 0.0) * M_PI / 180.0);
    }
    throw ApiError(Status::BadRequest, "A rotation needs 'q' or 'axis' and 'angle'");
}

Base::Quantity quantityFrom(const Json& value)
{
    if (value.is_number()) {
        return Base::Quantity(value.get<double>());
    }
    if (value.is_string()) {
        return Base::Quantity::parse(value.get<std::string>());
    }
    if (value.contains("text") && value["text"].is_string() && !value.contains("value")) {
        return Base::Quantity::parse(value["text"].get<std::string>());
    }
    const double number = value.value("value", 0.0);
    const std::string unit = value.value("unit", std::string());
    if (unit.empty()) {
        return Base::Quantity(number);
    }
    // Let FreeCAD's parser work out the unit, then keep the number as given (internal units).
    Base::Quantity parsed = Base::Quantity::parse("1 " + unit);
    return Base::Quantity(number * parsed.getValue(), parsed.getUnit());
}

App::DocumentObject* objectFrom(const Json& value, App::Document* doc)
{
    App::Document* target = doc;
    if (value.contains("doc") && value["doc"].is_string()) {
        target = App::GetApplication().getDocument(value["doc"].get<std::string>().c_str());
    }
    const std::string name = value.value("name", std::string());
    App::DocumentObject* obj = target ? target->getObject(name.c_str()) : nullptr;
    if (!obj) {
        throw ApiError(Status::NotFound, "No object '" + name + "'");
    }
    return obj;
}

}  // namespace

Json objectRef(const App::DocumentObject* object)
{
    return {
        {"$type", "Object"},
        {"doc", object->getDocument() ? object->getDocument()->getName() : ""},
        {"name", object->getNameInDocument() ? object->getNameInDocument() : ""}
    };
}

Json pyToJson(PyObject* object, int depth)
{
    if (!object || object == Py_None) {
        return nullptr;
    }
    if (depth > maxDepth) {
        return {{"$type", "Repr"}, {"type", Py_TYPE(object)->tp_name}, {"repr", "<too deep>"}};
    }
    if (PyBool_Check(object)) {
        return object == Py_True;
    }
    if (PyLong_Check(object)) {
        int overflow = 0;
        const long long value = PyLong_AsLongLongAndOverflow(object, &overflow);
        if (overflow == 0) {
            return value;
        }
        return PyLong_AsDouble(object);
    }
    if (PyFloat_Check(object)) {
        const double value = PyFloat_AsDouble(object);
        if (!std::isfinite(value)) {
            return pyStr(object, false);
        }
        return value;
    }
    if (PyUnicode_Check(object)) {
        const char* utf8 = PyUnicode_AsUTF8(object);
        if (!utf8) {
            PyErr_Clear();
            return "";
        }
        return utf8;
    }
    if (PyBytes_Check(object)) {
        return makeBinary(PyBytes_AsString(object), static_cast<std::size_t>(PyBytes_Size(object)));
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        Json list = Json::array();
        const Py_ssize_t n = PySequence_Size(object);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* item = PySequence_GetItem(object, i);
            list.push_back(pyToJson(item, depth + 1));
            Py_XDECREF(item);
        }
        return list;
    }
    if (PyDict_Check(object)) {
        Json map = Json::object();
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(object, &pos, &key, &value)) {
            map[PyUnicode_Check(key) ? pyStr(key, false) : pyStr(key, true)]
                = pyToJson(value, depth + 1);
        }
        return map;
    }
    if (PyObject_TypeCheck(object, &Base::VectorPy::Type)) {
        const auto* v = static_cast<Base::VectorPy*>(object)->getVectorPtr();
        return {{"$type", "Vector"}, {"x", v->x}, {"y", v->y}, {"z", v->z}};
    }
    if (PyObject_TypeCheck(object, &Base::RotationPy::Type)) {
        return rotationJson(*static_cast<Base::RotationPy*>(object)->getRotationPtr());
    }
    if (PyObject_TypeCheck(object, &Base::PlacementPy::Type)) {
        const auto* p = static_cast<Base::PlacementPy*>(object)->getPlacementPtr();
        Json rot = rotationJson(p->getRotation());
        return {
            {"$type", "Placement"},
            {"base", vec3(p->getPosition())},
            {"rotation", rot["q"]},
            {"axis", rot["axis"]},
            {"angle", rot["angle"]}
        };
    }
    if (PyObject_TypeCheck(object, &Base::MatrixPy::Type)) {
        const auto* m = static_cast<Base::MatrixPy*>(object)->getMatrixPtr();
        Json a = Json::array();
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                a.push_back((*m)[r][c]);
            }
        }
        return {{"$type", "Matrix"}, {"a", a}};
    }
    if (PyObject_TypeCheck(object, &Base::QuantityPy::Type)) {
        const auto* q = static_cast<Base::QuantityPy*>(object)->getQuantityPtr();
        return {
            {"$type", "Quantity"},
            {"value", q->getValue()},
            {"unit", q->getUnit().getString()},
            {"text", q->getUserString()}
        };
    }
    if (PyObject_TypeCheck(object, &App::DocumentObjectPy::Type)) {
        auto* obj = static_cast<App::DocumentObjectPy*>(object)->getDocumentObjectPtr();
        if (obj && obj->isAttachedToDocument()) {
            return objectRef(obj);
        }
        return nullptr;
    }
    return {{"$type", "Repr"}, {"type", Py_TYPE(object)->tp_name}, {"repr", pyStr(object, true)}};
}

PyObject* jsonToPy(const Json& value, App::Document* doc)
{
    switch (value.type()) {
        case Json::value_t::null:
            Py_RETURN_NONE;
        case Json::value_t::boolean:
            return PyBool_FromLong(value.get<bool>() ? 1 : 0);
        case Json::value_t::number_integer:
            return PyLong_FromLongLong(value.get<long long>());
        case Json::value_t::number_unsigned:
            return PyLong_FromUnsignedLongLong(value.get<unsigned long long>());
        case Json::value_t::number_float:
            return PyFloat_FromDouble(value.get<double>());
        case Json::value_t::string:
            return PyUnicode_FromString(value.get_ref<const std::string&>().c_str());
        case Json::value_t::binary: {
            const auto& bin = value.get_binary();
            return PyBytes_FromStringAndSize(
                reinterpret_cast<const char*>(bin.data()),
                static_cast<Py_ssize_t>(bin.size())
            );
        }
        case Json::value_t::array: {
            PyObject* list = PyList_New(static_cast<Py_ssize_t>(value.size()));
            Py_ssize_t i = 0;
            for (const auto& item : value) {
                PyObject* converted = nullptr;
                try {
                    converted = jsonToPy(item, doc);
                }
                catch (...) {
                    Py_DECREF(list);
                    throw;
                }
                PyList_SET_ITEM(list, i++, converted);
            }
            return list;
        }
        case Json::value_t::object:
            break;
        default:
            Py_RETURN_NONE;
    }

    const std::string type = value.value("$type", std::string());
    if (type == "Vector") {
        return new Base::VectorPy(new Base::Vector3d(vecFrom(value)));
    }
    if (type == "Rotation") {
        return new Base::RotationPy(new Base::Rotation(rotationFrom(value)));
    }
    if (type == "Placement") {
        Base::Vector3d base = value.contains("base") ? vecFrom(value["base"]) : Base::Vector3d();
        Base::Rotation rot;
        if (value.contains("rotation")) {
            rot = rotationFrom(value["rotation"]);
        }
        else if (value.contains("axis")) {
            rot = rotationFrom(value);
        }
        return new Base::PlacementPy(new Base::Placement(base, rot));
    }
    if (type == "Matrix") {
        const Json& a = value.at("a");
        if (!a.is_array() || a.size() != 16) {
            throw ApiError(Status::BadRequest, "A matrix is 16 numbers, row major");
        }
        auto* m = new Base::Matrix4D();
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                (*m)[r][c] = a[r * 4 + c].get<double>();
            }
        }
        return new Base::MatrixPy(m);
    }
    if (type == "Quantity") {
        return new Base::QuantityPy(new Base::Quantity(quantityFrom(value)));
    }
    if (type == "Object") {
        return objectFrom(value, doc)->getPyObject();
    }
    if (type == "Repr") {
        throw ApiError(Status::BadRequest, "A Repr value is read only");
    }

    PyObject* dict = PyDict_New();
    for (const auto& [key, item] : value.items()) {
        PyObject* converted = nullptr;
        try {
            converted = jsonToPy(item, doc);
        }
        catch (...) {
            Py_DECREF(dict);
            throw;
        }
        PyDict_SetItemString(dict, key.c_str(), converted);
        Py_DECREF(converted);
    }
    return dict;
}

Json propertyInfo(const App::DocumentObject* object, App::Property* prop, bool withValue)
{
    Json info = Json::object();
    info["name"] = prop->getName();
    info["type"] = prop->getTypeId().getName();
    const char* group = prop->getGroup();
    info["group"] = group ? group : "";
    const char* doc = prop->getDocumentation();
    info["doc"] = doc ? doc : "";

    Json status = Json::array();
    const short attr = object->getPropertyType(prop);
    if ((attr & App::Prop_ReadOnly) || prop->testStatus(App::Property::ReadOnly)
        || prop->testStatus(App::Property::Immutable)) {
        status.push_back("ReadOnly");
    }
    if ((attr & App::Prop_Hidden) || prop->testStatus(App::Property::Hidden)) {
        status.push_back("Hidden");
    }
    if (attr & App::Prop_Output || prop->testStatus(App::Property::Output)) {
        status.push_back("Output");
    }
    if (attr & App::Prop_Transient || prop->testStatus(App::Property::Transient)) {
        status.push_back("Transient");
    }
    if (attr & App::Prop_NoRecompute || prop->testStatus(App::Property::NoRecompute)) {
        status.push_back("NoRecompute");
    }
    if (prop->testStatus(App::Property::PropDynamic)) {
        status.push_back("Dynamic");
    }
    info["status"] = status;

    if (auto* enumeration = freecad_cast<App::PropertyEnumeration*>(prop)) {
        info["enum"] = enumeration->getEnumVector();
    }
    if (auto* quantity = freecad_cast<App::PropertyQuantity*>(prop)) {
        info["unit"] = quantity->getUnit().getString();
    }

    for (const auto& [path, expression] : object->ExpressionEngine.getExpressions()) {
        if (expression && path.getPropertyName() == prop->getName()) {
            info["expression"] = expression->toString();
            if (path.toString() != prop->getName()) {
                info["expressionPath"] = path.toString();
            }
            break;
        }
    }

    if (withValue) {
        PyObject* value = prop->getPyObject();
        if (value) {
            info["value"] = pyToJson(value);
            Py_DECREF(value);
        }
        else {
            PyErr_Clear();
            info["value"] = nullptr;
        }
    }
    return info;
}

void setPropertyValue(App::DocumentObject* object, App::Property* prop, const Json& value)
{
    if (prop->testStatus(App::Property::Immutable)
        || (object->getPropertyType(prop) & App::Prop_ReadOnly)) {
        throw ApiError(Status::Forbidden, std::string("Property '") + prop->getName() + "' is read only");
    }
    PyObject* converted = jsonToPy(value, object->getDocument());
    try {
        prop->setPyObject(converted);
    }
    catch (...) {
        Py_DECREF(converted);
        throw;
    }
    Py_DECREF(converted);
}

}  // namespace Api
