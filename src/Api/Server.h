// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fastsignals/signal.h>

#include "ApiGlobal.h"
#include "Codec.h"

namespace App
{
class Document;
class DocumentObject;
class Property;
}  // namespace App

namespace Api
{

/// The `status` of a reply. See PROTOCOL.md.
enum class Status
{
    Ok,
    BadRequest,
    UnknownCommand,
    NotFound,
    TokenMismatch,
    Forbidden,
    Failed
};

ApiExport const char* statusName(Status status);

/// Thrown by a handler to answer with a status other than FAILED.
class ApiExport ApiError: public std::runtime_error
{
public:
    ApiError(Status status, const std::string& message)
        : std::runtime_error(message)
        , status(status)
    {}
    Status status;
};

struct ServerOptions
{
    /// Allow RunPython (and anything else that runs arbitrary code).
    bool allowPython = true;
    /// Publish events at all.
    bool events = true;
    /// Name of the transport serving the dispatcher: "ws", "stdio", "inproc".
    std::string transport = "inproc";
    /// Where clients reach it (ws://127.0.0.1:8765/, inproc://freecad, ...).
    std::string url = "inproc://freecad";
};

/// What a handler gets besides its params.
struct RequestContext
{
    std::string client;
};

using Handler = std::function<Json(const Json& params, RequestContext& context)>;

/**
 * The API dispatcher. It owns the command table, turns request bytes into reply bytes and
 * turns FreeCAD's application signals into events. It knows nothing about transports: the
 * WebSocket server, the stdio host and the C ABI all call dispatch() and add an event sink.
 *
 * There is one per process because FreeCAD's application (and its signals) is a singleton.
 * dispatch() must be called on FreeCAD's main thread; so are the event sinks.
 */
class ApiExport Server
{
public:
    static Server& instance();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    ServerOptions& options()
    {
        return m_options;
    }

    /// Random, fixed for the life of the process.
    const std::string& token() const
    {
        return m_token;
    }

    void registerCommand(const std::string& name, const std::string& description, Handler handler);
    bool hasCommand(const std::string& name) const;
    /// [{name, description}] in name order.
    Json describeCommands() const;

    /**
     * Run one request. `onReply` receives the encoded reply; events the request raised are
     * delivered to the sinks after it returns, so every transport sends the reply first.
     */
    void dispatch(
        const std::uint8_t* data,
        std::size_t size,
        const std::function<void(Bytes&&, Encoding)>& onReply
    );

    /// Convenience: dispatch and return the reply (events are still flushed after it is built).
    Bytes dispatch(const std::uint8_t* data, std::size_t size);

    /// The decoded form of dispatch(): request object in, reply object out. No events flushed.
    Json dispatchMessage(const Json& request);

    using EventSink = std::function<void(const Json& event)>;
    int addEventSink(EventSink sink);
    void removeEventSink(int id);

    /// Queue an event (inside a request) or deliver it now (outside one).
    void publish(const std::string& name, Json data);

    /// Changes whenever the geometry of `object` changes. For caching tessellations.
    std::uint64_t shapeRevision(const App::DocumentObject* object);

    /// Changed since it was created, opened or saved (App::Document keeps no such flag).
    bool isModified(const App::Document* doc) const;

private:
    Server();
    ~Server() = default;

    void connectSignals();
    void flushEvents();
    void deliver(Json event);

    struct Command
    {
        std::string description;
        Handler handler;
    };

    ServerOptions m_options;
    std::string m_token;
    std::map<std::string, Command> m_commands;

    std::map<int, EventSink> m_sinks;
    int m_nextSink = 1;
    std::uint64_t m_seq = 0;

    // Events raised while a request runs wait until its reply is out.
    bool m_inRequest = false;
    std::string m_currentClient;
    std::vector<Json> m_pending;
    std::set<std::pair<std::string, std::string>> m_changedSeen;  // coalesces ObjectChanged

    std::unordered_map<const App::DocumentObject*, std::uint64_t> m_shapeRevisions;
    std::uint64_t m_revisionCounter = 0;

    std::set<const App::Document*> m_modified;

    std::vector<fastsignals::scoped_connection> m_connections;
};

/// Look up by internal name; ApiError(NotFound) when missing.
ApiExport App::Document* requireDocument(const Json& params, const char* key = "doc");
ApiExport App::DocumentObject* requireObject(
    App::Document* doc,
    const Json& params,
    const char* key = "object"
);
ApiExport std::string requireString(const Json& params, const char* key);
ApiExport std::string optionalString(
    const Json& params,
    const char* key,
    const std::string& fallback = {}
);

// Handler groups, each in its own file.
void registerServerCommands(Server& server);
void registerDocumentCommands(Server& server);
void registerObjectCommands(Server& server);
void registerGeometryCommands(Server& server);
void registerScriptCommands(Server& server);

}  // namespace Api
