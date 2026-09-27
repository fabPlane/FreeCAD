// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ApiGlobal.h"

namespace Api
{

class Executor;
class Server;

struct WebSocketOptions
{
    /// ws://host:port/path. Port 0 picks a free port; the chosen one is in url() after start().
    std::string url = "ws://127.0.0.1:8765/";
    /// When set, a client must connect with ?key=<key> in the URL.
    std::string key;
    /**
     * Browser origins allowed to connect. A connection without an Origin header (a program,
     * not a web page) is always allowed. The default allows pages served from localhost and
     * 127.0.0.1 on any port; "*" allows every origin. Anything else is refused, so a random
     * web site cannot drive a local FreeCAD through the user's browser.
     */
    std::vector<std::string> allowedOrigins = {"http://localhost", "http://127.0.0.1"};
};

/**
 * The WebSocket transport: a Boost.Beast server on its own I/O thread. Text frames carry JSON,
 * binary frames CBOR. Every request is dispatched on FreeCAD's main thread through `executor`,
 * one at a time; events are pushed to every connection in the encoding it last used.
 */
class ApiExport WebSocketTransport
{
public:
    WebSocketTransport(Server& server, Executor& executor, WebSocketOptions options);
    ~WebSocketTransport();

    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;

    /// Bind and start accepting. Throws std::runtime_error when the address cannot be bound.
    void start();
    void stop();
    bool running() const;

    /// The URL clients dial, with the real port.
    std::string url() const;
    int connectionCount() const;

    class Impl;

private:
    std::shared_ptr<Impl> m_impl;
};

}  // namespace Api
