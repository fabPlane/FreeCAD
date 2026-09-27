// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>

#include <QCoreApplication>
#include <QSysInfo>

#include <Base/Interpreter.h>
#include <Base/Type.h>
#include <App/Application.h>
#include <App/DocumentObject.h>

#include "Server.h"

#ifdef _WIN32
# include <process.h>
# define getpid _getpid
#else
# include <unistd.h>
#endif

namespace Api
{

void registerServerCommands(Server& server)
{
    server.registerCommand("Ping", "Check that the server answers", [](const Json&, RequestContext&) {
        return Json(nullptr);
    });

    server.registerCommand(
        "GetVersion",
        "FreeCAD's version and the API protocol version",
        [](const Json&, RequestContext&) {
            auto& config = App::Application::Config();
            auto number = [&config](const char* key) {
                try {
                    return std::stoi(config[key]);
                }
                catch (...) {
                    return 0;
                }
            };
            const std::string full = config["BuildVersionMajor"] + "." + config["BuildVersionMinor"]
                + "." + config["BuildVersionPoint"] + config["BuildVersionSuffix"];
            return Json {
                {"major", number("BuildVersionMajor")},
                {"minor", number("BuildVersionMinor")},
                {"patch", number("BuildVersionPoint")},
                {"revision", config["BuildRevision"]},
                {"full", full},
                {"api", ProtocolVersion}
            };
        }
    );

    server.registerCommand(
        "GetServerInfo",
        "How this server is reached and what it allows",
        [&server](const Json&, RequestContext&) {
            const auto& options = server.options();
            return Json {
                {"token", server.token()},
                {"transport", options.transport},
                {"url", options.url},
                {"python", options.allowPython},
                {"events", options.events},
                {"pid", static_cast<long long>(getpid())},
#ifdef __EMSCRIPTEN__
                {"platform", "wasm"},
#else
                {"platform", QSysInfo::productType().toStdString()},
#endif
                {"gui", App::Application::Config()["RunMode"] == "Gui"},
                {"homePath", App::Application::getHomePath()},
                {"userDataPath", App::Application::getUserAppDataDir()}
            };
        }
    );

    server.registerCommand(
        "GetCommands",
        "Every command this server answers",
        [&server](const Json&, RequestContext&) { return server.describeCommands(); }
    );

    server.registerCommand(
        "GetTypes",
        "Registered types derived from 'base' (default App::DocumentObject) that can be created",
        [](const Json& params, RequestContext&) {
            const std::string baseName = optionalString(params, "base", "App::DocumentObject");
            const Base::Type base = Base::Type::fromName(baseName.c_str());
            if (base.isBad()) {
                throw ApiError(Status::NotFound, "No type '" + baseName + "'");
            }
            std::vector<Base::Type> types;
            Base::Type::getAllDerivedFrom(base, types);
            Json list = Json::array();
            for (const auto& type : types) {
                if (type.canInstantiate()) {
                    list.push_back(type.getName());
                }
            }
            std::sort(list.begin(), list.end());
            return list;
        }
    );

    server.registerCommand(
        "LoadModule",
        "Import a FreeCAD module (Part, PartDesign, Sketcher, ...)",
        [](const Json& params, RequestContext&) {
            const std::string name = requireString(params, "name");
            if (!Base::Interpreter().loadModule(name.c_str())) {
                throw ApiError(Status::NotFound, "Cannot load module '" + name + "'");
            }
            return Json(nullptr);
        }
    );
}

}  // namespace Api
