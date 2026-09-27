// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <Base/BoundBox.h>
#include <Base/Placement.h>
#include <Base/PlacementPy.h>
#include <App/ComplexGeoData.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <App/PropertyGeo.h>

#include "Server.h"
#include "Values.h"

namespace Api
{

namespace
{

const Data::ComplexGeoData* geometryOf(const App::DocumentObject* obj)
{
    auto* geo = freecad_cast<const App::GeoFeature*>(obj);
    if (!geo) {
        return nullptr;
    }
    const App::PropertyComplexGeoData* prop = geo->getPropertyOfGeometry();
    return prop ? prop->getComplexData() : nullptr;
}

std::vector<App::DocumentObject*> selectObjects(App::Document* doc, const Json& params)
{
    if (!params.contains("objects") || params["objects"].is_null()) {
        return doc->getObjects();
    }
    std::vector<App::DocumentObject*> objects;
    for (const auto& name : params["objects"]) {
        App::DocumentObject* obj = doc->getObject(name.get<std::string>().c_str());
        if (!obj) {
            throw ApiError(Status::NotFound, "No object '" + name.get<std::string>() + "'");
        }
        objects.push_back(obj);
    }
    return objects;
}

void appendPoint(std::vector<float>& out, const Base::Vector3d& p)
{
    out.push_back(static_cast<float>(p.x));
    out.push_back(static_cast<float>(p.y));
    out.push_back(static_cast<float>(p.z));
}

Json tessellate(
    Server& server,
    const App::DocumentObject* obj,
    const Data::ComplexGeoData& data,
    const Json& params
)
{
    const Base::BoundBox3d box = data.getBoundBox();
    double deflection = params.value("deflection", 0.0);
    if (deflection <= 0) {
        // FreeCAD's default: 0.1 % of the diagonal, with a floor for tiny parts.
        deflection = box.IsValid() ? std::max(box.CalcDiagonalLength() * 0.001, 1e-4) : 0.1;
    }

    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> faces;  // [firstTriangle, count] pairs
    std::vector<float> edgePositions;
    std::vector<std::uint32_t> edges;  // [firstPoint, count] pairs
    std::vector<float> vertices;

    // Meshing the whole shape once triangulates every face; the per-face reads below reuse it.
    std::vector<Base::Vector3d> allPoints;
    std::vector<Data::ComplexGeoData::Facet> allFacets;
    data.getFaces(allPoints, allFacets, deflection);

    const unsigned long faceCount = data.countSubElements("Face");
    if (faceCount > 0) {
        for (unsigned long i = 1; i <= faceCount; ++i) {
            std::unique_ptr<Data::Segment> segment(data.getSubElement("Face", i));
            std::vector<Base::Vector3d> points;
            std::vector<Base::Vector3d> normals;
            std::vector<Data::ComplexGeoData::Facet> facets;
            if (segment) {
                data.getFacesFromSubElement(segment.get(), points, normals, facets);
            }
            const auto base = static_cast<std::uint32_t>(positions.size() / 3);
            faces.push_back(static_cast<std::uint32_t>(indices.size() / 3));
            faces.push_back(static_cast<std::uint32_t>(facets.size()));
            for (const auto& p : points) {
                appendPoint(positions, p);
            }
            for (const auto& f : facets) {
                indices.push_back(base + f.I1);
                indices.push_back(base + f.I2);
                indices.push_back(base + f.I3);
            }
        }
    }
    else if (!allFacets.empty()) {
        // A mesh (or anything without B-rep faces) is one pickable face.
        for (const auto& p : allPoints) {
            appendPoint(positions, p);
        }
        for (const auto& f : allFacets) {
            indices.push_back(f.I1);
            indices.push_back(f.I2);
            indices.push_back(f.I3);
        }
        faces.push_back(0);
        faces.push_back(static_cast<std::uint32_t>(allFacets.size()));
    }

    if (params.value("edges", true)) {
        const unsigned long edgeCount = data.countSubElements("Edge");
        for (unsigned long i = 1; i <= edgeCount; ++i) {
            std::unique_ptr<Data::Segment> segment(data.getSubElement("Edge", i));
            std::vector<Base::Vector3d> points;
            std::vector<Data::ComplexGeoData::Line> lines;
            if (segment) {
                data.getLinesFromSubElement(segment.get(), points, lines);
            }
            edges.push_back(static_cast<std::uint32_t>(edgePositions.size() / 3));
            edges.push_back(static_cast<std::uint32_t>(points.size()));
            for (const auto& p : points) {
                appendPoint(edgePositions, p);
            }
        }

        const unsigned long vertexCount = data.countSubElements("Vertex");
        for (unsigned long i = 1; i <= vertexCount; ++i) {
            std::unique_ptr<Data::Segment> segment(data.getSubElement("Vertex", i));
            Base::Vector3d point;
            if (segment && data.getFirstVertexFromSubElement(segment.get(), point)) {
                appendPoint(vertices, point);
            }
            else {
                appendPoint(vertices, Base::Vector3d());
            }
        }
    }

    Json placement = nullptr;
    if (auto* geo = freecad_cast<const App::GeoFeature*>(obj)) {
        PyObject* py = new Base::PlacementPy(new Base::Placement(geo->Placement.getValue()));
        placement = pyToJson(py);
        Py_DECREF(py);
    }

    return {
        {"object", obj->getNameInDocument()},
        {"placement", placement},
        {"revision", server.shapeRevision(obj)},
        {"deflection", deflection},
        {"positions", makeBinary(positions)},
        {"indices", makeBinary(indices)},
        {"faces", makeBinary(faces)},
        {"edgePositions", makeBinary(edgePositions)},
        {"edges", makeBinary(edges)},
        {"vertices", makeBinary(vertices)}
    };
}

}  // namespace

void registerGeometryCommands(Server& server)
{
    server.registerCommand(
        "Tessellate",
        "Triangles, edges and vertices of objects' shapes, in the global frame, per sub-element",
        [&server](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            Json list = Json::array();
            for (auto* obj : selectObjects(doc, params)) {
                const Data::ComplexGeoData* data = geometryOf(obj);
                if (!data) {
                    continue;
                }
                list.push_back(tessellate(server, obj, *data, params));
            }
            return list;
        }
    );

    server.registerCommand(
        "GetBoundingBox",
        "Bounding box of objects' shapes",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            Base::BoundBox3d box;
            for (auto* obj : selectObjects(doc, params)) {
                if (const Data::ComplexGeoData* data = geometryOf(obj)) {
                    box.Add(data->getBoundBox());
                }
            }
            if (!box.IsValid()) {
                return Json(nullptr);
            }
            return Json {
                {"min", {box.MinX, box.MinY, box.MinZ}},
                {"max", {box.MaxX, box.MaxY, box.MaxZ}}
            };
        }
    );
}

}  // namespace Api
