// SPDX-License-Identifier: LGPL-2.1-or-later

// FreeCADApiServer: FreeCAD's application core with no GUI, serving the FreeCAD API
// (src/Api/PROTOCOL.md) over a WebSocket or over stdio. The web frontend (fab_cad) talks to it.

#include "../FCConfig.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
# include <windows.h>
#else
# include <csignal>
# include <pthread.h>
#endif

#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <App/Application.h>

#include <Api/Executor.h>
#include <Api/Host.h>
#include <Api/Server.h>
#include <Api/StdioTransport.h>
#include <Api/WebSocketTransport.h>

namespace
{

const char* usage
    = "Usage: FreeCADApiServer [options] [FILE.FCStd]\n"
      "\n"
      "Serve the FreeCAD API (see src/Api/PROTOCOL.md).\n"
      "\n"
      "  --listen URL         WebSocket address (default ws://127.0.0.1:8765/; port 0 = any free "
      "port)\n"
      "  --stdio              Serve on stdin/stdout (uint32 BE length frames), events on fd 3\n"
      "  --events-fd N        Events descriptor for --stdio (default 3; -1 for none)\n"
      "  --key KEY            Require ?key=KEY on WebSocket connections\n"
      "  --allow-origin O     Allow browser pages from origin O (repeatable; '*' for any).\n"
      "                       Default: http://localhost and http://127.0.0.1 on any port\n"
      "  --module NAME        Import a FreeCAD module at start (repeatable)\n"
      "  --no-python          Refuse RunPython\n"
      "  --no-events          Do not publish events\n"
      "  -h, --help           This text\n"
      "\n"
      "With --listen, the server prints one line 'FCAPI_READY <url>' on stdout once it accepts\n"
      "connections, so a supervisor can learn the port it got.\n";

struct Options
{
    std::string listen = "ws://127.0.0.1:8765/";
    bool stdio = false;
    int eventsFd = 3;
    std::string key;
    std::vector<std::string> origins;
    std::vector<std::string> modules;
    bool python = true;
    bool events = true;
    std::string file;
};

bool parse(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string(name) + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            std::cout << usage;
            return false;
        }
        if (arg == "--listen") {
            options.listen = value("--listen");
        }
        else if (arg == "--stdio") {
            options.stdio = true;
        }
        else if (arg == "--events-fd") {
            options.eventsFd = std::stoi(value("--events-fd"));
        }
        else if (arg == "--key") {
            options.key = value("--key");
        }
        else if (arg == "--allow-origin") {
            options.origins.push_back(value("--allow-origin"));
        }
        else if (arg == "--module") {
            options.modules.push_back(value("--module"));
        }
        else if (arg == "--no-python") {
            options.python = false;
        }
        else if (arg == "--no-events") {
            options.events = false;
        }
        else if (!arg.empty() && arg[0] == '-') {
            throw std::invalid_argument("Unknown option " + arg);
        }
        else {
            options.file = arg;
        }
    }
    return true;
}

/// Stop the main loop on Ctrl+C / SIGTERM, from a thread where that is safe.
void installStopHandler()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(
        [](DWORD) -> BOOL {
            Api::Executor::stopMainLoop();
            return TRUE;
        },
        TRUE
    );
#else
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);  // inherited by every thread started later
    std::thread([set]() {
        int signal = 0;
        sigwait(&set, &signal);
        Api::Executor::stopMainLoop();
    }).detach();
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        if (!parse(argc, argv, options)) {
            return 0;
        }
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << "\n\n" << usage;
        return 2;
    }

    if (options.stdio) {
        Api::StdioTransport::detachStdout();
    }
    else {
        installStopHandler();
    }

    // FreeCAD parses its own command line; give it none of ours.
    std::vector<char*> freecadArgs = {argv[0], nullptr};
    std::string error;
    if (!Api::initializeFreeCAD(1, freecadArgs.data(), error)) {
        std::cerr << error << std::endl;
        return 100;
    }

    Api::Server& server = Api::Server::instance();
    server.options().allowPython = options.python;
    server.options().events = options.events;

    try {
        Base::PyGILStateLocker lock;
        for (const auto& module : options.modules) {
            Base::Interpreter().loadModule(module.c_str());
        }
        if (!options.file.empty()) {
            App::GetApplication().openDocument(options.file.c_str());
        }
    }
    catch (const std::exception& e) {
        std::cerr << "FreeCADApiServer: " << e.what() << std::endl;
        Api::shutdownFreeCAD();
        return 1;
    }

    int status = 0;
    {
        // FreeCAD released the GIL after starting Python; each request takes it while it runs.
        if (options.stdio) {
            Api::StdioTransport stdio(server, options.eventsFd);
            status = stdio.run();
        }
        else {
            Api::WebSocketOptions wsOptions;
            wsOptions.url = options.listen;
            wsOptions.key = options.key;
            if (!options.origins.empty()) {
                wsOptions.allowedOrigins = options.origins;
            }
            Api::WebSocketTransport ws(server, Api::Executor::main(), wsOptions);
            try {
                ws.start();
            }
            catch (const std::exception& e) {
                std::cerr << "FreeCADApiServer: cannot listen on " << options.listen << ": "
                          << e.what() << std::endl;
                status = 1;
            }
            if (status == 0) {
                server.options().transport = "ws";
                server.options().url = ws.url();
                std::cout << "FCAPI_READY " << ws.url() << std::endl;
                Api::Executor::runMainLoop();
                ws.stop();
            }
        }
    }

    Api::shutdownFreeCAD();
    return status;
}
