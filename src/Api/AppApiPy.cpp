// SPDX-License-Identifier: LGPL-2.1-or-later

// The FreeCADApi Python module: start the API server inside a running FreeCAD.
//
//   import FreeCADApi
//   FreeCADApi.startServer("ws://127.0.0.1:8765/")   # desktop FreeCAD: served by Qt's loop
//   FreeCADApi.serve("ws://127.0.0.1:8765/")         # FreeCADCmd: blocks until stopServer()
//   FreeCADApi.dispatch(b'{"id":1,"cmd":"Ping"}')    # in-process, no transport
//   FreeCADApi.commands()

#include <Python.h>

#include <memory>
#include <string>
#include <vector>

#include <Base/Console.h>

#include "Codec.h"
#include "Executor.h"
#include "Server.h"
#include "WebSocketTransport.h"

namespace
{

std::unique_ptr<Api::WebSocketTransport> transport;

bool startTransport(const char* url, const char* key, PyObject* origins, bool python)
{
    auto& server = Api::Server::instance();
    Api::WebSocketOptions options;
    options.url = url;
    options.key = key ? key : "";
    if (origins && origins != Py_None) {
        options.allowedOrigins.clear();
        PyObject* iterator = PyObject_GetIter(origins);
        if (!iterator) {
            return false;
        }
        while (PyObject* item = PyIter_Next(iterator)) {
            const char* text = PyUnicode_AsUTF8(item);
            if (text) {
                options.allowedOrigins.emplace_back(text);
            }
            Py_DECREF(item);
        }
        Py_DECREF(iterator);
        if (PyErr_Occurred()) {
            return false;
        }
    }
    if (transport) {
        transport->stop();
    }
    server.options().allowPython = python;
    transport = std::make_unique<Api::WebSocketTransport>(server, Api::Executor::main(), options);
    try {
        transport->start();
    }
    catch (const std::exception& e) {
        transport.reset();
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return false;
    }
    server.options().transport = "ws";
    server.options().url = transport->url();
    Base::Console().message("FreeCADApi: listening on {}\n", transport->url());
    return true;
}

PyObject* startServer(PyObject*, PyObject* args, PyObject* kwargs)
{
    const char* url = "ws://127.0.0.1:8765/";
    const char* key = "";
    PyObject* origins = Py_None;
    int python = 1;
    static const char* keywords[] = {"url", "key", "origins", "python", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "|ssOp",
            const_cast<char**>(keywords),
            &url,
            &key,
            &origins,
            &python
        )) {
        return nullptr;
    }
    if (!startTransport(url, key, origins, python != 0)) {
        return nullptr;
    }
    return PyUnicode_FromString(transport->url().c_str());
}

PyObject* serve(PyObject* self, PyObject* args, PyObject* kwargs)
{
    PyObject* url = startServer(self, args, kwargs);
    if (!url) {
        return nullptr;
    }
    Py_DECREF(url);
    // Serve on this thread until stopServer(); release the GIL while waiting for work.
    Py_BEGIN_ALLOW_THREADS Api::Executor::runMainLoop();
    Py_END_ALLOW_THREADS Py_RETURN_NONE;
}

PyObject* stopServer(PyObject*, PyObject*)
{
    if (transport) {
        transport->stop();
        transport.reset();
    }
    Api::Executor::stopMainLoop();
    Py_RETURN_NONE;
}

PyObject* serverUrl(PyObject*, PyObject*)
{
    if (!transport) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(transport->url().c_str());
}

PyObject* dispatch(PyObject*, PyObject* args)
{
    Py_buffer buffer;
    if (!PyArg_ParseTuple(args, "y*", &buffer)) {
        return nullptr;
    }
    Api::Bytes reply;
    try {
        reply = Api::Server::instance().dispatch(
            static_cast<const std::uint8_t*>(buffer.buf),
            static_cast<std::size_t>(buffer.len)
        );
    }
    catch (const std::exception& e) {
        PyBuffer_Release(&buffer);
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
    PyBuffer_Release(&buffer);
    return PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(reply.data()),
        static_cast<Py_ssize_t>(reply.size())
    );
}

PyObject* commands(PyObject*, PyObject*)
{
    const Api::Json list = Api::Server::instance().describeCommands();
    PyObject* out = PyList_New(0);
    for (const auto& item : list) {
        PyObject* name = PyUnicode_FromString(item["name"].get<std::string>().c_str());
        PyList_Append(out, name);
        Py_DECREF(name);
    }
    return out;
}

PyMethodDef methods[] = {
    {"startServer",
     reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(startServer)),
     METH_VARARGS | METH_KEYWORDS,
     "startServer(url='ws://127.0.0.1:8765/', key='', origins=None, python=True) -> url\n"
     "Serve the API on a WebSocket; requests run in Qt's event loop (desktop FreeCAD)."},
    {"serve",
     reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(serve)),
     METH_VARARGS | METH_KEYWORDS,
     "serve(url, key='', origins=None, python=True)\n"
     "Like startServer, then run requests on this thread until stopServer() (FreeCADCmd)."},
    {"stopServer", stopServer, METH_NOARGS, "Stop the WebSocket server."},
    {"serverUrl", serverUrl, METH_NOARGS, "The URL being served, or None."},
    {"dispatch", dispatch, METH_VARARGS, "dispatch(request: bytes) -> bytes. One request, in process."},
    {"commands", commands, METH_NOARGS, "Names of every API command."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "FreeCADApi",
    "The FreeCAD API server (see src/Api/PROTOCOL.md).",
    -1,
    methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

}  // namespace

PyMODINIT_FUNC PyInit_FreeCADApi()
{
    return PyModule_Create(&module);
}
