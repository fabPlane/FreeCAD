// SPDX-License-Identifier: LGPL-2.1-or-later

#include "StdioTransport.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
# include <fcntl.h>
# include <io.h>
# define fc_read _read
# define fc_write _write
# define fc_dup _dup
# define fc_dup2 _dup2
#else
# include <fcntl.h>
# include <unistd.h>
# define fc_read ::read
# define fc_write ::write
# define fc_dup ::dup
# define fc_dup2 ::dup2
#endif

#include "Codec.h"
#include "Server.h"

namespace Api
{

namespace
{

bool readAll(int fd, std::uint8_t* data, std::size_t size)
{
    std::size_t done = 0;
    while (done < size) {
        const auto n = fc_read(fd, data + done, static_cast<unsigned>(size - done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t size)
{
    std::size_t done = 0;
    while (done < size) {
        const auto n = fc_write(fd, data + done, static_cast<unsigned>(size - done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool writeFrame(int fd, const Bytes& payload)
{
    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::uint8_t header[4] = {
        static_cast<std::uint8_t>(size >> 24),
        static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 8),
        static_cast<std::uint8_t>(size)
    };
    return writeAll(fd, header, 4) && writeAll(fd, payload.data(), payload.size());
}

bool fdIsOpen(int fd)
{
#ifdef _WIN32
    return _get_osfhandle(fd) != -1;
#else
    return fcntl(fd, F_GETFD) != -1;
#endif
}

}  // namespace

int StdioTransport::s_replyFd = -1;

int StdioTransport::detachStdout()
{
    if (s_replyFd < 0) {
        std::fflush(stdout);
#ifdef _WIN32
        _setmode(0, _O_BINARY);
        _setmode(1, _O_BINARY);
#endif
#ifdef _WIN32
        s_replyFd = fc_dup(1);
#else
        // Well above 3, so the reply pipe can never be mistaken for the events descriptor.
        s_replyFd = fcntl(1, F_DUPFD_CLOEXEC, 10);
#endif
        fc_dup2(2, 1);
    }
    return s_replyFd;
}

StdioTransport::StdioTransport(Server& server, int eventsFd)
    : m_server(server)
    , m_eventsFd(eventsFd)
{}

int StdioTransport::run()
{
    // Look for the events descriptor before anything else can open one with its number.
    const bool events = m_eventsFd >= 0 && m_eventsFd != s_replyFd && fdIsOpen(m_eventsFd);
    // Keep the pipe to the parent for frames only.
    const int replyFd = detachStdout();
    // Events go out in the encoding the client last used.
    Encoding lastEncoding = Encoding::Json;
    int sink = 0;
    if (events) {
        sink = m_server.addEventSink([this, &lastEncoding](const Json& event) {
            writeFrame(m_eventsFd, encode(event, lastEncoding));
        });
    }

    m_server.options().transport = "stdio";
    m_server.options().url = "stdio://";

    int status = 0;
    for (;;) {
        std::uint8_t header[4];
        if (!readAll(0, header, 4)) {
            break;  // stdin closed: the parent wants us gone
        }
        const std::uint32_t size = (std::uint32_t(header[0]) << 24)
            | (std::uint32_t(header[1]) << 16) | (std::uint32_t(header[2]) << 8)
            | std::uint32_t(header[3]);
        Bytes request(size);
        if (size > 0 && !readAll(0, request.data(), size)) {
            status = 1;
            break;
        }
        bool written = true;
        m_server.dispatch(request.data(), request.size(), [&](Bytes&& reply, Encoding encoding) {
            lastEncoding = encoding;
            written = writeFrame(replyFd, reply);
        });
        if (!written) {
            status = 1;
            break;
        }
    }

    if (events) {
        m_server.removeEventSink(sink);
    }
    return status;
}

}  // namespace Api
