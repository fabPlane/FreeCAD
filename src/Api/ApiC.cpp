// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ApiC.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <Base/Interpreter.h>
#include <App/Application.h>
#include <App/Document.h>

#include "Codec.h"
#include "Host.h"
#include "Server.h"

#ifdef __EMSCRIPTEN__
# include <emscripten.h>

// The loader installs Module.__fcapiEvent before calling fcapi_init.
EM_JS(void, fcapi_emit_event_js, (const uint8_t* data, size_t size), {
    if (Module["__fcapiEvent"]) {
        Module["__fcapiEvent"](HEAPU8.subarray(data, data + size));
    }
});
#endif

namespace
{

std::string lastError;
int eventSink = 0;
Api::Encoding eventEncoding = Api::Encoding::Cbor;
fcapi_event_callback eventCallback = nullptr;
void* eventUser = nullptr;

void setEnv(const std::string& name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

void emitEvent(const Api::Json& event)
{
    const Api::Bytes bytes = Api::encode(event, eventEncoding);
#ifdef __EMSCRIPTEN__
    fcapi_emit_event_js(bytes.data(), bytes.size());
#endif
    if (eventCallback) {
        eventCallback(bytes.data(), bytes.size(), eventUser);
    }
}

}  // namespace

extern "C" {

FCAPI_EXPORT int fcapi_init(const char* config_json)
{
    lastError.clear();
    Api::Json config = Api::Json::object();
    if (config_json && *config_json) {
        try {
            config = Api::Json::parse(config_json);
        }
        catch (const std::exception& e) {
            lastError = std::string("Bad config: ") + e.what();
            return 1;
        }
    }

    if (config.contains("env") && config["env"].is_object()) {
        for (const auto& [name, value] : config["env"].items()) {
            if (value.is_string()) {
                setEnv(name, value.get<std::string>());
            }
        }
    }

    std::string argv0 = config.value("argv0", std::string("FreeCADApi"));
    std::vector<char*> argv = {argv0.data(), nullptr};
    if (!Api::initializeFreeCAD(1, argv.data(), lastError)) {
        return 2;
    }

    Api::Server& server = Api::Server::instance();
    server.options().allowPython = config.value("python", true);
    server.options().events = config.value("events", true);
    server.options().transport = "inproc";
    server.options().url = "inproc://freecad";
    eventEncoding = config.value("eventEncoding", std::string("cbor")) == "json"
        ? Api::Encoding::Json
        : Api::Encoding::Cbor;
    if (!eventSink) {
        eventSink = server.addEventSink(emitEvent);
    }

    try {
        Base::PyGILStateLocker lock;
        if (config.contains("modules") && config["modules"].is_array()) {
            for (const auto& module : config["modules"]) {
                Base::Interpreter().loadModule(module.get<std::string>().c_str());
            }
        }
        const std::string preload = config.value("preload", std::string());
        if (!preload.empty()) {
            App::GetApplication().openDocument(preload.c_str());
        }
    }
    catch (const Base::Exception& e) {
        lastError = e.what();
        return 3;
    }
    catch (const std::exception& e) {
        lastError = e.what();
        return 3;
    }
    return 0;
}

FCAPI_EXPORT uint8_t* fcapi_dispatch(const uint8_t* request, size_t size, size_t* out_size)
{
    lastError.clear();
    if (out_size) {
        *out_size = 0;
    }
    if (!request || !out_size) {
        lastError = "fcapi_dispatch needs a request and out_size";
        return nullptr;
    }
    try {
        Api::Bytes reply = Api::Server::instance().dispatch(request, size);
        auto* out = static_cast<uint8_t*>(std::malloc(reply.empty() ? 1 : reply.size()));
        if (!out) {
            lastError = "out of memory";
            return nullptr;
        }
        std::memcpy(out, reply.data(), reply.size());
        *out_size = reply.size();
        return out;
    }
    catch (const std::exception& e) {
        lastError = e.what();
        return nullptr;
    }
}

FCAPI_EXPORT void fcapi_free(void* reply)
{
    std::free(reply);
}

FCAPI_EXPORT void fcapi_shutdown(void)
{
    if (eventSink) {
        Api::Server::instance().removeEventSink(eventSink);
        eventSink = 0;
    }
    Api::shutdownFreeCAD();
}

FCAPI_EXPORT const char* fcapi_last_error(void)
{
    return lastError.c_str();
}

FCAPI_EXPORT void fcapi_set_event_callback(fcapi_event_callback callback, void* user)
{
    eventCallback = callback;
    eventUser = user;
}

}  // extern "C"
