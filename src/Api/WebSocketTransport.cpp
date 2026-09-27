// SPDX-License-Identifier: LGPL-2.1-or-later

#include "WebSocketTransport.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <Base/Console.h>

#include "Codec.h"
#include "Executor.h"
#include "Server.h"

namespace Api
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

struct Endpoint
{
    std::string host;
    unsigned short port = 0;
    std::string path = "/";
};

Endpoint parseUrl(const std::string& url)
{
    const std::string scheme = "ws://";
    if (url.rfind(scheme, 0) != 0) {
        throw std::runtime_error(
            "The WebSocket transport takes ws://host:port/ URLs, not '" + url + "'"
        );
    }
    std::string rest = url.substr(scheme.size());
    Endpoint out;
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        out.path = rest.substr(slash);
        rest = rest.substr(0, slash);
    }
    std::string portText;
    if (!rest.empty() && rest.front() == '[') {  // [::1]:8765
        const auto close = rest.find(']');
        if (close == std::string::npos) {
            throw std::runtime_error("Bad IPv6 address in '" + url + "'");
        }
        out.host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':') {
            portText = rest.substr(close + 2);
        }
    }
    else {
        const auto colon = rest.rfind(':');
        out.host = rest.substr(0, colon);
        if (colon != std::string::npos) {
            portText = rest.substr(colon + 1);
        }
    }
    if (out.host.empty()) {
        out.host = "127.0.0.1";
    }
    if (!portText.empty()) {
        const int port = std::stoi(portText);
        if (port < 0 || port > 65535) {
            throw std::runtime_error("Bad port in '" + url + "'");
        }
        out.port = static_cast<unsigned short>(port);
    }
    return out;
}

bool originAllowed(const std::vector<std::string>& allowed, const std::string& origin)
{
    if (origin.empty()) {
        return true;  // not a browser
    }
    for (const auto& entry : allowed) {
        if (entry == "*" || entry == origin) {
            return true;
        }
        // "http://localhost" allows "http://localhost:5173".
        if (origin.size() > entry.size() && origin.compare(0, entry.size(), entry) == 0
            && origin[entry.size()] == ':') {
            return true;
        }
    }
    return false;
}

std::string queryValue(const std::string& target, const std::string& name)
{
    const auto question = target.find('?');
    if (question == std::string::npos) {
        return {};
    }
    std::string query = target.substr(question + 1);
    std::size_t start = 0;
    while (start <= query.size()) {
        auto end = query.find('&', start);
        if (end == std::string::npos) {
            end = query.size();
        }
        const std::string pair = query.substr(start, end - start);
        const auto eq = pair.find('=');
        if (pair.substr(0, eq) == name) {
            return eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        }
        start = end + 1;
    }
    return {};
}

}  // namespace

class Session;

class WebSocketTransport::Impl: public std::enable_shared_from_this<WebSocketTransport::Impl>
{
public:
    Impl(Server& server, Executor& executor, WebSocketOptions options)
        : server(server)
        , executor(executor)
        , options(std::move(options))
        , acceptor(ioc)
    {}

    void start();
    void stop();
    void accept();
    void broadcast(const Json& event);
    void forget(const Session* session);

    Server& server;
    Executor& executor;
    WebSocketOptions options;
    net::io_context ioc {1};
    tcp::acceptor acceptor;
    std::thread thread;
    std::atomic<bool> running {false};
    std::string url;
    int sinkId = 0;

    mutable std::mutex mutex;
    std::vector<std::weak_ptr<Session>> sessions;
};

class Session: public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket&& socket, std::shared_ptr<WebSocketTransport::Impl> impl)
        : m_ws(std::move(socket))
        , m_impl(std::move(impl))
    {}

    void start()
    {
        http::async_read(
            m_ws.next_layer(),
            m_buffer,
            m_request,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                self->onHandshakeRequest(ec);
            }
        );
    }

    /// Queue one frame. Thread safe.
    void send(std::shared_ptr<const Bytes> data, bool text)
    {
        if (!m_impl->running) {
            return;
        }
        net::post(m_ws.get_executor(), [self = shared_from_this(), data = std::move(data), text]() {
            self->m_queue.push_back({data, text});
            if (self->m_queue.size() == 1) {
                self->writeNext();
            }
        });
    }

    Encoding encoding() const
    {
        return m_encoding.load();
    }

    void close()
    {
        net::post(m_ws.get_executor(), [self = shared_from_this()]() {
            beast::error_code ignored;
            beast::get_lowest_layer(self->m_ws).socket().close(ignored);
        });
    }

private:
    void refuse(http::status status, const std::string& why)
    {
        auto response
            = std::make_shared<http::response<http::string_body>>(status, m_request.version());
        response->set(http::field::content_type, "text/plain");
        response->body() = why + "\n";
        response->prepare_payload();
        http::async_write(
            m_ws.next_layer(),
            *response,
            [self = shared_from_this(), response](beast::error_code, std::size_t) {
                beast::error_code ignored;
                self->m_ws.next_layer().socket().shutdown(tcp::socket::shutdown_send, ignored);
                self->m_impl->forget(self.get());
            }
        );
    }

    void onHandshakeRequest(beast::error_code ec)
    {
        if (ec) {
            m_impl->forget(this);
            return;
        }
        if (!websocket::is_upgrade(m_request)) {
            refuse(http::status::upgrade_required, "This is the FreeCAD API; connect with a WebSocket");
            return;
        }
        const std::string origin(m_request[http::field::origin]);
        if (!originAllowed(m_impl->options.allowedOrigins, origin)) {
            refuse(http::status::forbidden, "Origin '" + origin + "' is not allowed");
            return;
        }
        const std::string target(m_request.target());
        if (!m_impl->options.key.empty() && queryValue(target, "key") != m_impl->options.key) {
            refuse(http::status::forbidden, "Wrong or missing key");
            return;
        }

        m_ws.read_message_max(std::size_t(1) << 30);
        m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        m_ws.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
            res.set(http::field::server, "FreeCADApi");
        }));
        m_ws.async_accept(m_request, [self = shared_from_this()](beast::error_code ec) {
            if (ec) {
                self->m_impl->forget(self.get());
                return;
            }
            self->read();
        });
    }

    void read()
    {
        m_ws.async_read(m_buffer, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->onRead(ec);
        });
    }

    void onRead(beast::error_code ec)
    {
        if (ec) {
            m_impl->forget(this);
            return;
        }
        auto data = std::make_shared<Bytes>(
            net::buffers_begin(m_buffer.data()),
            net::buffers_end(m_buffer.data())
        );
        m_buffer.consume(m_buffer.size());

        // Dispatch on the main thread, in arrival order; the reply comes back here.
        std::weak_ptr<WebSocketTransport::Impl> weakImpl = m_impl;
        m_impl->executor.post([self = shared_from_this(), weakImpl, data]() {
            auto impl = weakImpl.lock();
            if (!impl || !impl->running) {
                return;
            }
            impl->server.dispatch(data->data(), data->size(), [&self](Bytes&& reply, Encoding encoding) {
                self->m_encoding = encoding;
                self->send(std::make_shared<const Bytes>(std::move(reply)), encoding == Encoding::Json);
            });
        });
        read();
    }

    void writeNext()
    {
        const auto& front = m_queue.front();
        m_ws.text(front.text);
        m_ws.async_write(
            net::buffer(*front.data),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    self->m_queue.clear();
                    self->m_impl->forget(self.get());
                    return;
                }
                self->m_queue.pop_front();
                if (!self->m_queue.empty()) {
                    self->writeNext();
                }
            }
        );
    }

    struct Frame
    {
        std::shared_ptr<const Bytes> data;
        bool text;
    };

    websocket::stream<beast::tcp_stream> m_ws;
    std::shared_ptr<WebSocketTransport::Impl> m_impl;
    beast::flat_buffer m_buffer;
    http::request<http::string_body> m_request;
    std::deque<Frame> m_queue;
    std::atomic<Encoding> m_encoding {Encoding::Json};
};

void WebSocketTransport::Impl::start()
{
    const Endpoint endpoint = parseUrl(options.url);

    tcp::resolver resolver(ioc);
    const auto results = resolver.resolve(endpoint.host, std::to_string(endpoint.port));
    if (results.empty()) {
        throw std::runtime_error("Cannot resolve '" + endpoint.host + "'");
    }
    const tcp::endpoint bindTo = results.begin()->endpoint();

    acceptor.open(bindTo.protocol());
    acceptor.set_option(net::socket_base::reuse_address(true));
    acceptor.bind(bindTo);
    acceptor.listen(net::socket_base::max_listen_connections);

    const auto bound = acceptor.local_endpoint();
    const std::string host = bound.address().is_v6() ? "[" + bound.address().to_string() + "]"
                                                     : bound.address().to_string();
    url = "ws://" + host + ":" + std::to_string(bound.port()) + endpoint.path;

    std::weak_ptr<Impl> weak = shared_from_this();
    sinkId = server.addEventSink([weak](const Json& event) {
        if (auto self = weak.lock()) {
            self->broadcast(event);
        }
    });

    running = true;
    accept();
    thread = std::thread([this]() {
        try {
            ioc.run();
        }
        catch (const std::exception& e) {
            Base::Console().error("FreeCADApi: WebSocket I/O stopped: {}\n", e.what());
        }
    });
}

void WebSocketTransport::Impl::accept()
{
    acceptor.async_accept(
        net::make_strand(ioc),
        [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                return;  // closed
            }
            auto session = std::make_shared<Session>(std::move(socket), self);
            {
                std::lock_guard lock(self->mutex);
                self->sessions.push_back(session);
            }
            session->start();
            self->accept();
        }
    );
}

void WebSocketTransport::Impl::broadcast(const Json& event)
{
    std::vector<std::shared_ptr<Session>> live;
    {
        std::lock_guard lock(mutex);
        for (const auto& weak : sessions) {
            if (auto session = weak.lock()) {
                live.push_back(std::move(session));
            }
        }
    }
    std::shared_ptr<const Bytes> json;
    std::shared_ptr<const Bytes> cbor;
    for (const auto& session : live) {
        if (session->encoding() == Encoding::Cbor) {
            if (!cbor) {
                cbor = std::make_shared<const Bytes>(encode(event, Encoding::Cbor));
            }
            session->send(cbor, false);
        }
        else {
            if (!json) {
                json = std::make_shared<const Bytes>(encode(event, Encoding::Json));
            }
            session->send(json, true);
        }
    }
}

void WebSocketTransport::Impl::forget(const Session* session)
{
    std::lock_guard lock(mutex);
    sessions.erase(
        std::remove_if(
            sessions.begin(),
            sessions.end(),
            [session](const std::weak_ptr<Session>& weak) {
                auto live = weak.lock();
                return !live || live.get() == session;
            }
        ),
        sessions.end()
    );
}

void WebSocketTransport::Impl::stop()
{
    if (!running.exchange(false)) {
        return;
    }
    server.removeEventSink(sinkId);
    net::post(ioc, [self = shared_from_this()]() {
        beast::error_code ignored;
        self->acceptor.close(ignored);
        std::vector<std::shared_ptr<Session>> live;
        {
            std::lock_guard lock(self->mutex);
            for (const auto& weak : self->sessions) {
                if (auto session = weak.lock()) {
                    live.push_back(std::move(session));
                }
            }
            self->sessions.clear();
        }
        for (const auto& session : live) {
            session->close();
        }
    });
    if (thread.joinable()) {
        // Give connections a moment to close, then stop whatever is left.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!ioc.stopped() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ioc.stop();
        thread.join();
    }
}

WebSocketTransport::WebSocketTransport(Server& server, Executor& executor, WebSocketOptions options)
    : m_impl(std::make_shared<Impl>(server, executor, std::move(options)))
{}

WebSocketTransport::~WebSocketTransport()
{
    stop();
}

void WebSocketTransport::start()
{
    m_impl->start();
}

void WebSocketTransport::stop()
{
    m_impl->stop();
}

bool WebSocketTransport::running() const
{
    return m_impl->running;
}

std::string WebSocketTransport::url() const
{
    return m_impl->url;
}

int WebSocketTransport::connectionCount() const
{
    std::lock_guard lock(m_impl->mutex);
    return static_cast<int>(std::count_if(
        m_impl->sessions.begin(),
        m_impl->sessions.end(),
        [](const std::weak_ptr<Session>& weak) { return !weak.expired(); }
    ));
}

}  // namespace Api
