// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

#include <Base/FileInfo.h>
#include <Base/Interpreter.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>

#include "Paths.h"
#include "Server.h"
#include "Transaction.h"
#include "Values.h"

namespace Api
{

namespace
{

/// Owns one Python reference.
struct PyRef
{
    PyObject* p = nullptr;
    explicit PyRef(PyObject* object = nullptr)
        : p(object)
    {}
    ~PyRef()
    {
        Py_XDECREF(p);
    }
    PyRef(const PyRef&) = delete;
    PyRef& operator=(const PyRef&) = delete;
    explicit operator bool() const
    {
        return p != nullptr;
    }
};

std::string pythonErrorText()
{
    // Format the pending exception the way the console shows it, then clear it.
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    std::string text;
    PyRef module(PyImport_ImportModule("traceback"));
    if (module) {
        PyRef lines(PyObject_CallMethod(
            module.p,
            "format_exception",
            "OOO",
            type ? type : Py_None,
            value ? value : Py_None,
            traceback ? traceback : Py_None
        ));
        if (lines) {
            PyRef empty(PyUnicode_FromString(""));
            PyRef joined(PyUnicode_Join(empty.p, lines.p));
            if (joined) {
                const char* utf8 = PyUnicode_AsUTF8(joined.p);
                text = utf8 ? utf8 : "";
            }
        }
    }
    PyErr_Clear();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    return text;
}

std::string stringIOValue(PyObject* buffer)
{
    PyRef value(PyObject_CallMethod(buffer, "getvalue", nullptr));
    if (!value) {
        PyErr_Clear();
        return {};
    }
    const char* utf8 = PyUnicode_AsUTF8(value.p);
    return utf8 ? utf8 : "";
}

Json runPython(const std::string& code, const std::string& mode)
{
    PyObject* main = PyImport_AddModule("__main__");  // borrowed
    PyObject* globals = PyModule_GetDict(main);       // borrowed

    PyRef io(PyImport_ImportModule("io"));
    PyRef out(io ? PyObject_CallMethod(io.p, "StringIO", nullptr) : nullptr);
    PyRef err(io ? PyObject_CallMethod(io.p, "StringIO", nullptr) : nullptr);
    if (!out || !err) {
        throw ApiError(Status::Failed, pythonErrorText());
    }

    // Choose how to compile: "auto" evaluates an expression and falls back to exec.
    PyRef compiled;
    bool isExpression = false;
    if (mode == "eval" || mode == "auto") {
        compiled.p = Py_CompileString(code.c_str(), "<fab_cad>", Py_eval_input);
        isExpression = compiled.p != nullptr;
        if (!compiled && mode == "eval") {
            return {{"stdout", ""}, {"stderr", ""}, {"exception", pythonErrorText()}};
        }
        if (!compiled) {
            PyErr_Clear();
        }
    }
    if (!compiled) {
        compiled.p = Py_CompileString(code.c_str(), "<fab_cad>", Py_file_input);
        if (!compiled) {
            return {{"stdout", ""}, {"stderr", ""}, {"exception", pythonErrorText()}};
        }
    }

    PyRef oldOut(PySys_GetObject("stdout"));  // borrowed; take our own reference
    Py_XINCREF(oldOut.p);
    PyRef oldErr(PySys_GetObject("stderr"));
    Py_XINCREF(oldErr.p);
    PySys_SetObject("stdout", out.p);
    PySys_SetObject("stderr", err.p);

    PyRef result(PyEval_EvalCode(compiled.p, globals, globals));
    std::string exception;
    if (!result) {
        exception = pythonErrorText();
    }

    PySys_SetObject("stdout", oldOut.p ? oldOut.p : Py_None);
    PySys_SetObject("stderr", oldErr.p ? oldErr.p : Py_None);

    Json reply = {{"stdout", stringIOValue(out.p)}, {"stderr", stringIOValue(err.p)}};
    if (!exception.empty()) {
        reply["exception"] = exception;
    }
    else if (isExpression && result.p != Py_None) {
        reply["result"] = pyToJson(result.p);
        PyRef repr(PyObject_Repr(result.p));
        const char* utf8 = repr ? PyUnicode_AsUTF8(repr.p) : nullptr;
        reply["repr"] = utf8 ? utf8 : "";
        PyErr_Clear();
    }
    return reply;
}

/**
 * The modules registered for a file type, then Part for the B-rep formats it reads and writes
 * itself. A build without a registered module (the WebAssembly one has no Import module)
 * still handles STEP, IGES, BREP and STL through Part.
 */
std::vector<std::string> ioModules(std::vector<std::string> registered, const std::string& ext)
{
    static const std::set<std::string> partFormats
        = {"step", "stp", "iges", "igs", "brep", "brp", "stl"};
    if (partFormats.contains(ext)
        && std::find(registered.begin(), registered.end(), "Part") == registered.end()) {
        registered.emplace_back("Part");
    }
    return registered;
}

/// Import the first of `modules` that loads; ApiError when none does.
PyObject* importFirst(const std::vector<std::string>& modules, const std::string& what)
{
    std::string errors;
    for (const auto& name : modules) {
        if (PyObject* module = PyImport_ImportModule(name.c_str())) {
            return module;
        }
        errors += pythonErrorText();
    }
    throw ApiError(Status::Failed, "No module for " + what + " could be loaded\n" + errors);
}

std::string extensionOf(const std::string& fileName)
{
    std::string ext = Base::FileInfo(fileName).extension();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

}  // namespace

void registerScriptCommands(Server& server)
{
    server.registerCommand(
        "RunPython",
        "Run Python in FreeCAD's __main__ namespace (mode exec, eval or auto)",
        [&server](const Json& params, RequestContext&) {
            if (!server.options().allowPython) {
                throw ApiError(Status::Forbidden, "This server was started with --no-python");
            }
            return runPython(requireString(params, "code"), optionalString(params, "mode", "exec"));
        }
    );

    server.registerCommand(
        "Import",
        "Import a file (STEP, IGES, BREP, STL, ...) into a document, by path or bytes",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            std::string path = optionalString(params, "path");
            if (path.empty()) {
                if (!params.contains("data") || !params["data"].is_binary()) {
                    throw ApiError(Status::BadRequest, "Give 'path' or 'data' and 'fileName'");
                }
                const std::string fileName = utf8FromPath(
                    pathFromUtf8(requireString(params, "fileName")).filename()
                );
                path = makeTempDir("import") + "/" + fileName;
                std::ofstream file(pathFromUtf8(path), std::ios::binary);
                const auto& data = params["data"].get_binary();
                file.write(
                    reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size())
                );
            }
            const auto modules = ioModules(
                App::GetApplication().getImportModules(extensionOf(path)),
                extensionOf(path)
            );
            if (modules.empty()) {
                throw ApiError(Status::BadRequest, "No importer for '" + extensionOf(path) + "' files");
            }
            std::set<App::DocumentObject*> before;
            for (auto* obj : doc->getObjects()) {
                before.insert(obj);
            }
            AutoTransaction transaction(doc, "Import");
            PyRef mod(importFirst(modules, "importing '" + extensionOf(path) + "'"));
            PyRef result(PyObject_CallMethod(mod.p, "insert", "ss", path.c_str(), doc->getName()));
            if (!result) {
                throw ApiError(Status::Failed, pythonErrorText());
            }
            Json created = Json::array();
            for (auto* obj : doc->getObjects()) {
                if (!before.contains(obj)) {
                    created.push_back(obj->getNameInDocument());
                }
            }
            return created;
        }
    );

    server.registerCommand(
        "Export",
        "Export objects to a file format and return the bytes",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            std::string format = requireString(params, "format");
            std::transform(format.begin(), format.end(), format.begin(), ::tolower);
            const auto modules = ioModules(App::GetApplication().getExportModules(format), format);
            if (modules.empty()) {
                throw ApiError(Status::BadRequest, "No exporter for '" + format + "'");
            }

            PyRef list(PyList_New(0));
            if (params.contains("objects") && params["objects"].is_array()) {
                for (const auto& name : params["objects"]) {
                    App::DocumentObject* obj = doc->getObject(name.get<std::string>().c_str());
                    if (!obj) {
                        throw ApiError(Status::NotFound, "No object '" + name.get<std::string>() + "'");
                    }
                    PyRef py(obj->getPyObject());
                    PyList_Append(list.p, py.p);
                }
            }
            else {
                throw ApiError(Status::BadRequest, "'objects' must be a list of object names");
            }

            const std::string fileName = std::string(doc->getName()) + "." + format;
            const std::string dir = makeTempDir("export");
            const std::string path = dir + "/" + fileName;
            PyRef mod(importFirst(modules, "exporting '" + format + "'"));
            PyRef result(PyObject_CallMethod(mod.p, "export", "Os", list.p, path.c_str()));
            if (!result) {
                throw ApiError(Status::Failed, pythonErrorText());
            }
            std::ifstream in(pathFromUtf8(path), std::ios::binary);
            if (!in) {
                throw ApiError(Status::Failed, "The exporter wrote no file");
            }
            Bytes data {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            in.close();
            std::filesystem::remove_all(pathFromUtf8(dir));
            return Json {{"data", Json::binary(std::move(data))}, {"fileName", fileName}};
        }
    );
}

}  // namespace Api
