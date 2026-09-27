// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Server.h"

#include <random>
#include <sstream>

#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/PropertyGeo.h>

namespace Api
{

const char* statusName(Status status)
{
    switch (status) {
        case Status::Ok:
            return "OK";
        case Status::BadRequest:
            return "BAD_REQUEST";
        case Status::UnknownCommand:
            return "UNKNOWN_COMMAND";
        case Status::NotFound:
            return "NOT_FOUND";
        case Status::TokenMismatch:
            return "TOKEN_MISMATCH";
        case Status::Forbidden:
            return "FORBIDDEN";
        case Status::Failed:
            return "FAILED";
    }
    return "FAILED";
}

namespace
{

std::string makeToken()
{
    std::random_device device;
    std::mt19937_64 engine(device());
    std::ostringstream out;
    out << std::hex << engine() << engine();
    return out.str();
}

const char* docName(const App::Document& doc)
{
    return doc.getName();
}

}  // namespace

Server& Server::instance()
{
    static Server server;
    return server;
}

Server::Server()
    : m_token(makeToken())
{
    registerServerCommands(*this);
    registerDocumentCommands(*this);
    registerObjectCommands(*this);
    registerGeometryCommands(*this);
    registerScriptCommands(*this);
    connectSignals();
}

void Server::registerCommand(const std::string& name, const std::string& description, Handler handler)
{
    m_commands[name] = Command {description, std::move(handler)};
}

bool Server::hasCommand(const std::string& name) const
{
    return m_commands.contains(name);
}

Json Server::describeCommands() const
{
    Json list = Json::array();
    for (const auto& [name, command] : m_commands) {
        list.push_back({{"name", name}, {"description", command.description}});
    }
    return list;
}

Json Server::dispatchMessage(const Json& request)
{
    Json reply = Json::object();
    reply["id"] = request.is_object() && request.contains("id") ? request["id"] : Json(nullptr);
    reply["token"] = m_token;

    auto fail = [&reply](Status status, const std::string& message) {
        reply["status"] = statusName(status);
        reply["error"] = message;
        return reply;
    };

    if (!request.is_object() || !request.contains("cmd") || !request["cmd"].is_string()) {
        return fail(Status::BadRequest, "A request is an object with a string 'cmd'");
    }
    const std::string cmd = request["cmd"].get<std::string>();

    if (request.contains("token") && request["token"].is_string()) {
        const auto& token = request["token"].get_ref<const std::string&>();
        if (!token.empty() && token != m_token) {
            return fail(Status::TokenMismatch, "This server's token is " + m_token);
        }
    }

    const auto it = m_commands.find(cmd);
    if (it == m_commands.end()) {
        return fail(Status::UnknownCommand, "Unknown command '" + cmd + "'");
    }

    Json params = request.contains("params") ? request["params"] : Json::object();
    if (params.is_null()) {
        params = Json::object();
    }
    if (!params.is_object()) {
        return fail(Status::BadRequest, "'params' must be an object");
    }

    RequestContext context;
    if (request.contains("client") && request["client"].is_string()) {
        context.client = request["client"].get<std::string>();
    }

    try {
        Base::PyGILStateLocker lock;
        Json result = it->second.handler(params, context);
        reply["status"] = statusName(Status::Ok);
        reply["result"] = std::move(result);
        return reply;
    }
    catch (const ApiError& e) {
        return fail(e.status, e.what());
    }
    catch (const Json::exception& e) {
        return fail(Status::BadRequest, e.what());
    }
    catch (const Base::Exception& e) {
        return fail(Status::Failed, e.what());
    }
    catch (const Py::Exception&) {
        Base::PyGILStateLocker lock;
        Base::PyException error;
        return fail(Status::Failed, error.what());
    }
    catch (const std::exception& e) {
        return fail(Status::Failed, e.what());
    }
    catch (...) {
        return fail(Status::Failed, "Unknown error in " + cmd);
    }
}

void Server::dispatch(
    const std::uint8_t* data,
    std::size_t size,
    const std::function<void(Bytes&&, Encoding)>& onReply
)
{
    const Encoding encoding = detectEncoding(data, size);
    Json reply;

    m_inRequest = true;
    try {
        Json request = decode(data, size, encoding);
        m_currentClient = request.is_object() && request.contains("client")
                && request["client"].is_string()
            ? request["client"].get<std::string>()
            : std::string();
        reply = dispatchMessage(request);
    }
    catch (const std::exception& e) {
        reply = {
            {"id", nullptr},
            {"token", m_token},
            {"status", statusName(Status::BadRequest)},
            {"error", std::string("Malformed message: ") + e.what()}
        };
    }
    m_inRequest = false;
    m_currentClient.clear();

    onReply(encode(reply, encoding), encoding);
    flushEvents();
}

Bytes Server::dispatch(const std::uint8_t* data, std::size_t size)
{
    Bytes out;
    dispatch(data, size, [&out](Bytes&& reply, Encoding) { out = std::move(reply); });
    return out;
}

int Server::addEventSink(EventSink sink)
{
    const int id = m_nextSink++;
    m_sinks.emplace(id, std::move(sink));
    return id;
}

void Server::removeEventSink(int id)
{
    m_sinks.erase(id);
}

void Server::publish(const std::string& name, Json data)
{
    if (!m_options.events) {
        return;
    }
    if (m_inRequest) {
        if (name == "ObjectChanged") {
            auto key = std::make_pair(
                data.value("object", std::string()),
                data.value("property", std::string())
            );
            key.first = data.value("doc", std::string()) + "#" + key.first;
            if (!m_changedSeen.insert(key).second) {
                return;
            }
        }
        if (!m_currentClient.empty()) {
            data["client"] = m_currentClient;
        }
        m_pending.push_back({{"event", name}, {"data", std::move(data)}});
        return;
    }
    deliver({{"event", name}, {"data", std::move(data)}});
}

void Server::flushEvents()
{
    auto pending = std::move(m_pending);
    m_pending.clear();
    m_changedSeen.clear();
    for (auto& event : pending) {
        deliver(std::move(event));
    }
}

void Server::deliver(Json event)
{
    event["seq"] = ++m_seq;
    // A sink may remove itself while we iterate.
    auto sinks = m_sinks;
    for (auto& [id, sink] : sinks) {
        (void)id;
        sink(event);
    }
}

std::uint64_t Server::shapeRevision(const App::DocumentObject* object)
{
    auto it = m_shapeRevisions.find(object);
    if (it == m_shapeRevisions.end()) {
        it = m_shapeRevisions.emplace(object, ++m_revisionCounter).first;
    }
    return it->second;
}

bool Server::isModified(const App::Document* doc) const
{
    return m_modified.contains(doc);
}

void Server::connectSignals()
{
    auto& app = App::GetApplication();

    auto objectData = [](const App::DocumentObject& obj) {
        Json data = Json::object();
        data["doc"] = obj.getDocument() ? obj.getDocument()->getName() : "";
        data["object"] = obj.getNameInDocument() ? obj.getNameInDocument() : "";
        return data;
    };

    m_connections.emplace_back(app.signalNewDocument.connect([this](const App::Document& doc, bool) {
        publish("DocumentCreated", {{"doc", docName(doc)}, {"label", doc.Label.getValue()}});
    }));
    m_connections.emplace_back(app.signalDeleteDocument.connect([this](const App::Document& doc) {
        // Forget cached revisions of objects that are about to go away with the document.
        for (auto* obj : doc.getObjects()) {
            m_shapeRevisions.erase(obj);
        }
        m_modified.erase(&doc);
        publish("DocumentDeleted", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(app.signalRelabelDocument.connect([this](const App::Document& doc) {
        publish("DocumentRenamed", {{"doc", docName(doc)}, {"label", doc.Label.getValue()}});
    }));
    m_connections.emplace_back(app.signalActiveDocument.connect([this](const App::Document& doc) {
        publish("ActiveDocumentChanged", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(app.signalFinishSaveDocument.connect(
        [this](const App::Document& doc, const std::string& file) {
            m_modified.erase(&doc);
            publish("DocumentSaved", {{"doc", docName(doc)}, {"fileName", file}});
        }
    ));
    m_connections.emplace_back(app.signalFinishRestoreDocument.connect([this](const App::Document& doc) {
        m_modified.erase(&doc);
        publish("DocumentRestored", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(
        app.signalNewObject.connect([this, objectData](const App::DocumentObject& obj) {
            if (!obj.getDocument()->testStatus(App::Document::Restoring)) {
                m_modified.insert(obj.getDocument());
            }
            Json data = objectData(obj);
            data["type"] = obj.getTypeId().getName();
            publish("ObjectCreated", std::move(data));
        })
    );
    m_connections.emplace_back(
        app.signalDeletedObject.connect([this, objectData](const App::DocumentObject& obj) {
            m_shapeRevisions.erase(&obj);
            if (obj.getDocument()) {
                m_modified.insert(obj.getDocument());
            }
            publish("ObjectDeleted", objectData(obj));
        })
    );
    m_connections.emplace_back(app.signalChangedObject.connect(
        [this, objectData](const App::DocumentObject& obj, const App::Property& prop) {
            if (prop.isDerivedFrom<App::PropertyComplexGeoData>()) {
                m_shapeRevisions[&obj] = ++m_revisionCounter;
            }
            const char* name = prop.getName();
            if (!name || !obj.getNameInDocument()) {
                return;
            }
            if (!obj.getDocument()->testStatus(App::Document::Restoring)
                && !prop.testStatus(App::Property::Transient)) {
                m_modified.insert(obj.getDocument());
            }
            Json data = objectData(obj);
            data["property"] = name;
            publish("ObjectChanged", std::move(data));
        }
    ));
    m_connections.emplace_back(
        app.signalObjectRecomputed.connect([this, objectData](const App::DocumentObject& obj) {
            publish("ObjectRecomputed", objectData(obj));
        })
    );
    m_connections.emplace_back(app.signalRecomputed.connect([this](const App::Document& doc) {
        publish("Recomputed", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(
        app.signalOpenTransaction.connect([this](const App::Document& doc, std::string name) {
            publish("TransactionOpened", {{"doc", docName(doc)}, {"name", std::move(name)}});
        })
    );
    m_connections.emplace_back(app.signalCommitTransaction.connect([this](const App::Document& doc) {
        publish("TransactionCommitted", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(app.signalAbortTransaction.connect([this](const App::Document& doc) {
        publish("TransactionAborted", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(app.signalUndoDocument.connect([this](const App::Document& doc) {
        publish("Undo", {{"doc", docName(doc)}});
    }));
    m_connections.emplace_back(app.signalRedoDocument.connect([this](const App::Document& doc) {
        publish("Redo", {{"doc", docName(doc)}});
    }));
}

App::Document* requireDocument(const Json& params, const char* key)
{
    std::string name;
    if (params.contains(key) && params[key].is_string()) {
        name = params[key].get<std::string>();
    }
    App::Document* doc = name.empty() ? App::GetApplication().getActiveDocument()
                                      : App::GetApplication().getDocument(name.c_str());
    if (!doc) {
        throw ApiError(
            Status::NotFound,
            name.empty() ? "No active document" : "No document '" + name + "'"
        );
    }
    return doc;
}

App::DocumentObject* requireObject(App::Document* doc, const Json& params, const char* key)
{
    const std::string name = requireString(params, key);
    App::DocumentObject* obj = doc->getObject(name.c_str());
    if (!obj) {
        throw ApiError(Status::NotFound, "No object '" + name + "' in document '" + doc->getName() + "'");
    }
    return obj;
}

std::string requireString(const Json& params, const char* key)
{
    if (!params.contains(key) || !params[key].is_string()) {
        throw ApiError(Status::BadRequest, std::string("Missing string parameter '") + key + "'");
    }
    return params[key].get<std::string>();
}

std::string optionalString(const Json& params, const char* key, const std::string& fallback)
{
    if (params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

}  // namespace Api
