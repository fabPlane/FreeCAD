// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "ApiGlobal.h"

namespace Api
{

class Server;

/**
 * The stdio transport, the same framing as the KiCad fork's native host:
 *
 *  - stdin carries requests, stdout replies, fd 3 events (when the parent opened it);
 *  - every frame is a uint32 big-endian length followed by that many bytes;
 *  - strictly one request in flight, answered in order.
 *
 * FreeCAD writes its console to stdout, so run() first moves the real stdout to a private
 * descriptor and points fd 1 at stderr: nothing but frames reaches the parent's pipe.
 * run() returns when stdin closes. It runs on the calling (main) thread.
 */
class ApiExport StdioTransport
{
public:
    explicit StdioTransport(Server& server, int eventsFd = 3);

    /**
     * Move the real stdout to a new descriptor (returned) and point fd 1 at stderr. Call it
     * before FreeCAD starts so not even start-up messages reach the parent's pipe; run() does
     * it itself when nobody did.
     */
    static int detachStdout();

    int run();

private:
    Server& m_server;
    int m_eventsFd;
    static int s_replyFd;
};

}  // namespace Api
