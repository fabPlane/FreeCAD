// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <map>
#include <set>

#include <Base/Type.h>
#include <App/Application.h>
#include <App/ComplexGeoData.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/Expression.h>
#include <App/GeoFeature.h>
#include <App/GroupExtension.h>
#include <App/ObjectIdentifier.h>
#include <App/OriginGroupExtension.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>

#include "Server.h"
#include "Transaction.h"
#include "Values.h"

namespace Api
{

namespace
{

/**
 * What the desktop tree nests under an object. In FreeCAD that is decided by the view provider
 * (ViewProvider::claimChildren), which does not exist without the GUI, so the rules of the
 * common view providers are repeated here by the link properties they claim.
 */
const std::vector<std::pair<const char*, std::vector<const char*>>>& claimRules()
{
    static const std::vector<std::pair<const char*, std::vector<const char*>>> rules = {
        {"Part::Boolean", {"Base", "Tool"}},
        {"Part::MultiFuse", {"Shapes"}},
        {"Part::MultiCommon", {"Shapes"}},
        {"Part::Extrusion", {"Base"}},
        {"Part::Revolution", {"Source"}},
        {"Part::Mirroring", {"Source"}},
        {"Part::FilletBase", {"Base"}},
        {"Part::Offset", {"Source"}},
        {"Part::Thickness", {"Faces"}},
        {"Part::Loft", {"Sections"}},
        {"Part::Sweep", {"Sections"}},
        {"Part::Compound", {"Links"}},
        {"Part::RuledSurface", {"Curve1", "Curve2"}},
        {"Part::Refine", {"Source"}},
        {"Part::Reverse", {"Source"}},
        {"PartDesign::ProfileBased", {"Profile"}},
        {"PartDesign::Transformed", {}},
    };
    return rules;
}

std::vector<App::DocumentObject*> claimedChildren(const App::DocumentObject* obj)
{
    std::vector<App::DocumentObject*> children;
    auto add = [&children, obj](App::DocumentObject* child) {
        if (child && child != obj && child->isAttachedToDocument()
            && std::find(children.begin(), children.end(), child) == children.end()) {
            children.push_back(child);
        }
    };

    if (auto* origin = obj->getExtensionByType<App::OriginGroupExtension>(true)) {
        add(origin->Origin.getValue());
    }
    if (auto* group = obj->getExtensionByType<App::GroupExtension>(true)) {
        for (auto* child : group->Group.getValues()) {
            add(child);
        }
    }

    for (const auto& [typeName, properties] : claimRules()) {
        const Base::Type type = Base::Type::fromName(typeName);
        if (type.isBad() || !obj->getTypeId().isDerivedFrom(type)) {
            continue;
        }
        for (const char* name : properties) {
            auto* link = freecad_cast<App::PropertyLinkBase*>(obj->getPropertyByName(name));
            if (!link) {
                continue;
            }
            std::vector<App::DocumentObject*> linked;
            link->getLinks(linked);
            for (auto* child : linked) {
                add(child);
            }
        }
    }
    return children;
}

Json names(const std::vector<App::DocumentObject*>& objects)
{
    Json list = Json::array();
    for (auto* obj : objects) {
        if (obj && obj->getNameInDocument()) {
            list.push_back(obj->getNameInDocument());
        }
    }
    return list;
}

Json typeHierarchy(Base::Type type)
{
    Json list = Json::array();
    while (!type.isBad() && type != Base::Type::fromName("Base::BaseClass")) {
        list.push_back(type.getName());
        type = type.getParent();
    }
    return list;
}

const Data::ComplexGeoData* geometryOf(const App::DocumentObject* obj)
{
    auto* geo = freecad_cast<const App::GeoFeature*>(obj);
    if (!geo) {
        return nullptr;
    }
    const App::PropertyComplexGeoData* prop = geo->getPropertyOfGeometry();
    return prop ? prop->getComplexData() : nullptr;
}

Json objectInfo(
    const App::DocumentObject* obj,
    const std::map<const App::DocumentObject*, std::vector<App::DocumentObject*>>* parents
)
{
    Json info = Json::object();
    info["name"] = obj->getNameInDocument();
    info["label"] = obj->Label.getValue();
    info["type"] = obj->getTypeId().getName();
    info["typeHierarchy"] = typeHierarchy(obj->getTypeId());
    info["isGeo"] = obj->isDerivedFrom<App::GeoFeature>();
    info["isValid"] = obj->isValid();
    info["isTouched"] = obj->isTouched();
    info["isError"] = obj->isError();
    info["status"] = obj->getStatusString();
    info["inList"] = names(obj->getInList());
    info["outList"] = names(obj->getOutList());
    info["children"] = names(claimedChildren(obj));
    info["visibility"] = obj->Visibility.getValue();

    if (parents) {
        auto it = parents->find(obj);
        info["parents"] = it == parents->end() ? Json::array() : names(it->second);
    }

    if (const auto* data = geometryOf(obj)) {
        const Base::BoundBox3d box = data->getBoundBox();
        if (box.IsValid()) {
            info["bbox"]
                = {{"min", {box.MinX, box.MinY, box.MinZ}}, {"max", {box.MaxX, box.MaxY, box.MaxZ}}};
        }
    }
    return info;
}

std::map<const App::DocumentObject*, std::vector<App::DocumentObject*>> parentMap(App::Document* doc)
{
    std::map<const App::DocumentObject*, std::vector<App::DocumentObject*>> parents;
    for (auto* obj : doc->getObjects()) {
        for (auto* child : claimedChildren(obj)) {
            parents[child].push_back(obj);
        }
    }
    return parents;
}

App::Property* requireProperty(App::DocumentObject* obj, const std::string& name)
{
    App::Property* prop = obj->getPropertyByName(name.c_str());
    if (!prop) {
        throw ApiError(
            Status::NotFound,
            "No property '" + name + "' on '" + obj->getNameInDocument() + "'"
        );
    }
    return prop;
}

Json propertyList(App::DocumentObject* obj, const Json& wanted)
{
    Json list = Json::array();
    if (wanted.is_array()) {
        for (const auto& name : wanted) {
            list.push_back(propertyInfo(obj, requireProperty(obj, name.get<std::string>())));
        }
        return list;
    }
    std::vector<std::pair<const char*, App::Property*>> props;
    obj->getPropertyNamedList(props);
    for (const auto& [name, prop] : props) {
        (void)name;
        list.push_back(propertyInfo(obj, prop));
    }
    return list;
}

void loadModuleOf(const std::string& type)
{
    // Asking with loadModule imports the module that defines it ("Part" for "Part::Box").
    (void)Base::Type::getTypeIfDerivedFrom(type, Base::Type::fromName("App::DocumentObject"), true);
}

}  // namespace

void registerObjectCommands(Server& server)
{
    server.registerCommand(
        "GetObjects",
        "Every object of a document, in document order",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            const auto parents = parentMap(doc);
            Json list = Json::array();
            for (auto* obj : doc->getObjects()) {
                list.push_back(objectInfo(obj, &parents));
            }
            return list;
        }
    );

    server.registerCommand(
        "GetObject",
        "One object (with its properties if asked)",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            const auto parents = parentMap(doc);
            Json info = objectInfo(obj, &parents);
            if (params.value("properties", false)) {
                info["properties"] = propertyList(obj, nullptr);
            }
            return info;
        }
    );

    server.registerCommand(
        "GetProperties",
        "Properties of an object, all or by name",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            return propertyList(obj, params.contains("names") ? params["names"] : Json(nullptr));
        }
    );

    server.registerCommand(
        "SetProperties",
        "Set property values (one undo step unless a transaction is open)",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            if (!params.contains("values") || !params["values"].is_object()) {
                throw ApiError(Status::BadRequest, "'values' must be an object");
            }
            // Check every name first so a typo does not leave half the values set.
            std::vector<std::pair<App::Property*, const Json*>> changes;
            for (const auto& [name, value] : params["values"].items()) {
                changes.emplace_back(requireProperty(obj, name), &value);
            }
            AutoTransaction transaction(doc, "Edit " + std::string(obj->Label.getValue()));
            Json changed = Json::array();
            for (const auto& [prop, value] : changes) {
                setPropertyValue(obj, prop, *value);
            }
            for (const auto& [prop, value] : changes) {
                (void)value;
                changed.push_back(propertyInfo(obj, prop));
            }
            return changed;
        }
    );

    server.registerCommand(
        "SetExpression",
        "Bind a property (path) to an expression, or clear it with null",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            const std::string path = requireString(params, "path");
            const App::ObjectIdentifier id = App::ObjectIdentifier::parse(obj, path);
            AutoTransaction transaction(doc, "Set expression");
            if (!params.contains("expression") || params["expression"].is_null()) {
                obj->clearExpression(id);
            }
            else {
                const std::string text = requireString(params, "expression");
                std::shared_ptr<App::Expression> expr(App::Expression::parse(obj, text));
                obj->setExpression(id, expr);
            }
            return Json(nullptr);
        }
    );

    server.registerCommand(
        "AddObject",
        "Create an object of a registered type, optionally inside a group or body",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            const std::string type = requireString(params, "type");
            loadModuleOf(type);
            const Base::Type base = Base::Type::fromName("App::DocumentObject");
            const Base::Type t = Base::Type::fromName(type.c_str());
            if (t.isBad() || !t.isDerivedFrom(base) || !t.canInstantiate()) {
                throw ApiError(Status::NotFound, "No document object type '" + type + "'");
            }
            App::DocumentObject* group = nullptr;
            if (params.contains("group") && params["group"].is_string()) {
                group = requireObject(doc, params, "group");
                if (!group->hasExtension(App::GroupExtension::getExtensionClassTypeId())) {
                    throw ApiError(
                        Status::BadRequest,
                        std::string("'") + group->getNameInDocument() + "' is not a group"
                    );
                }
            }
            // Like FreeCAD's own commands: "Part::Box" is called "Box" (then "Box001", ...).
            std::string shortName(t.getName());
            if (const auto colons = shortName.rfind("::"); colons != std::string::npos) {
                shortName = shortName.substr(colons + 2);
            }
            const std::string name = optionalString(params, "name", shortName);

            AutoTransaction transaction(doc, "Create " + name);
            App::DocumentObject* obj = doc->addObject(type, name.c_str());
            if (!obj) {
                throw ApiError(Status::Failed, "Cannot create '" + type + "'");
            }
            if (params.contains("label") && params["label"].is_string()) {
                obj->Label.setValue(params["label"].get<std::string>());
            }
            if (group) {
                group->getExtensionByType<App::GroupExtension>()->addObject(obj);
            }
            if (params.contains("properties") && params["properties"].is_object()) {
                for (const auto& [propName, value] : params["properties"].items()) {
                    setPropertyValue(obj, requireProperty(obj, propName), value);
                }
            }
            const auto parents = parentMap(doc);
            return objectInfo(obj, &parents);
        }
    );

    server.registerCommand(
        "RemoveObject",
        "Delete an object (and, with recursive, what it groups)",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            AutoTransaction transaction(doc, "Delete " + std::string(obj->Label.getValue()));
            if (params.value("recursive", false)) {
                if (auto* group = obj->getExtensionByType<App::GroupExtension>(true)) {
                    group->removeObjectsFromDocument();
                }
            }
            doc->removeObject(obj->getNameInDocument());
            return Json(nullptr);
        }
    );

    server.registerCommand(
        "AddProperty",
        "Add a dynamic property",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            const std::string type = requireString(params, "type");
            const std::string name = requireString(params, "name");
            const std::string group = optionalString(params, "group", "Base");
            const std::string tooltip = optionalString(params, "documentation");
            AutoTransaction transaction(doc, "Add property " + name);
            App::Property* prop
                = obj->addDynamicProperty(type, name.c_str(), group.c_str(), tooltip.c_str());
            if (!prop) {
                throw ApiError(Status::Failed, "Cannot add property '" + name + "' of type " + type);
            }
            return propertyInfo(obj, prop);
        }
    );

    server.registerCommand(
        "RemoveProperty",
        "Remove a dynamic property",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::DocumentObject* obj = requireObject(doc, params);
            const std::string name = requireString(params, "name");
            requireProperty(obj, name);
            AutoTransaction transaction(doc, "Remove property " + name);
            if (!obj->removeDynamicProperty(name.c_str())) {
                throw ApiError(Status::Forbidden, "'" + name + "' is not a removable dynamic property");
            }
            return Json(nullptr);
        }
    );
}

}  // namespace Api
