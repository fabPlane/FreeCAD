// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <Base/FileInfo.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>

#include "Paths.h"
#include "Server.h"

namespace Api
{

namespace
{

Json documentInfo(Server& server, App::Document* doc)
{
    const auto undo = doc->getAvailableUndoNames();
    const auto redo = doc->getAvailableRedoNames();
    return {
        {"name", doc->getName()},
        {"label", doc->Label.getValue()},
        {"fileName", doc->FileName.getValue()},
        {"modified", server.isModified(doc)},
        {"active", App::GetApplication().getActiveDocument() == doc},
        {"transient", doc->testStatus(App::Document::TempDoc)},
        {"objectCount", doc->getObjects().size()},
        {"undoCount", undo.size()},
        {"redoCount", redo.size()}
    };
}

Json undoStack(App::Document* doc)
{
    return {{"undo", doc->getAvailableUndoNames()}, {"redo", doc->getAvailableRedoNames()}};
}

Bytes readFile(const std::string& path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) {
        throw ApiError(Status::Failed, "Cannot read '" + path + "'");
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const std::string& path, const Json::binary_t& data)
{
    std::ofstream out(pathFromUtf8(path), std::ios::binary);
    if (!out) {
        throw ApiError(Status::Failed, "Cannot write '" + path + "'");
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

/// A file name without directories, safe to put in the temp directory.
std::string plainFileName(const std::string& name, const std::string& fallback)
{
    std::string out = utf8FromPath(pathFromUtf8(name).filename());
    if (out.empty() || out == "." || out == "..") {
        out = fallback;
    }
    return out;
}

}  // namespace

void registerDocumentCommands(Server& server)
{
    server.registerCommand("ListDocuments", "Open documents", [&server](const Json&, RequestContext&) {
        Json list = Json::array();
        for (auto* doc : App::GetApplication().getDocuments()) {
            list.push_back(documentInfo(server, doc));
        }
        return list;
    });

    server.registerCommand(
        "NewDocument",
        "Create an empty document and make it active",
        [&server](const Json& params, RequestContext&) {
            const std::string name = optionalString(params, "name", "Unnamed");
            const std::string label = optionalString(params, "label");
            App::Document* doc = App::GetApplication().newDocument(
                name.c_str(),
                label.empty() ? nullptr : label.c_str()
            );
            App::GetApplication().setActiveDocument(doc);
            return documentInfo(server, doc);
        }
    );

    server.registerCommand(
        "OpenDocument",
        "Open an .FCStd file by path (on the server's file system)",
        [&server](const Json& params, RequestContext&) {
            const std::string path = requireString(params, "path");
            if (!Base::FileInfo(path).exists()) {
                throw ApiError(Status::NotFound, "No file '" + path + "'");
            }
            App::Document* doc = App::GetApplication().openDocument(path.c_str());
            if (!doc) {
                throw ApiError(Status::Failed, "Cannot open '" + path + "'");
            }
            App::GetApplication().setActiveDocument(doc);
            return documentInfo(server, doc);
        }
    );

    server.registerCommand(
        "OpenDocumentBytes",
        "Open a document from the bytes of an .FCStd file",
        [&server](const Json& params, RequestContext&) {
            if (!params.contains("data") || !params["data"].is_binary()) {
                throw ApiError(Status::BadRequest, "'data' must be bytes");
            }
            const std::string fileName
                = plainFileName(optionalString(params, "fileName", "Upload.FCStd"), "Upload.FCStd");
            const std::string dir = makeTempDir("upload");
            const std::string path = dir + "/" + fileName;
            writeFile(path, params["data"].get_binary());
            App::Document* doc = App::GetApplication().openDocument(path.c_str());
            if (!doc) {
                throw ApiError(Status::Failed, "Cannot open '" + fileName + "'");
            }
            App::GetApplication().setActiveDocument(doc);
            return documentInfo(server, doc);
        }
    );

    server.registerCommand(
        "SaveDocument",
        "Save a document to its file",
        [&server](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            if (std::string(doc->FileName.getValue()).empty()) {
                throw ApiError(Status::BadRequest, "The document has no file yet; use SaveDocumentAs");
            }
            if (!doc->save()) {
                throw ApiError(Status::Failed, "Saving failed");
            }
            return documentInfo(server, doc);
        }
    );

    server.registerCommand(
        "SaveDocumentAs",
        "Save a document to a new path (on the server's file system)",
        [&server](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            const std::string path = requireString(params, "path");
            if (!doc->saveAs(path.c_str())) {
                throw ApiError(Status::Failed, "Saving failed");
            }
            return documentInfo(server, doc);
        }
    );

    server.registerCommand(
        "SaveDocumentBytes",
        "The document as .FCStd bytes, without changing its file",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            std::string fileName = Base::FileInfo(doc->FileName.getValue()).fileName();
            if (fileName.empty()) {
                fileName = std::string(doc->getName()) + ".FCStd";
            }
            // saveCopy() insists on an .FCStd name, so write into a fresh directory.
            if (Base::FileInfo(fileName).extension().empty()) {
                fileName += ".FCStd";
            }
            const std::string dir = makeTempDir("save");
            const std::string path = dir + "/" + fileName;
            if (!doc->saveCopy(path.c_str())) {
                throw ApiError(Status::Failed, "Saving failed");
            }
            Bytes data = readFile(path);
            std::filesystem::remove_all(pathFromUtf8(dir));
            return Json {{"data", Json::binary(std::move(data))}, {"fileName", fileName}};
        }
    );

    server.registerCommand(
        "CloseDocument",
        "Close a document without saving",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            App::GetApplication().closeDocument(doc->getName());
            return Json(nullptr);
        }
    );

    server.registerCommand(
        "SetActiveDocument",
        "Make a document the active one",
        [](const Json& params, RequestContext&) {
            App::GetApplication().setActiveDocument(requireDocument(params));
            return Json(nullptr);
        }
    );

    server.registerCommand(
        "Recompute",
        "Recompute touched objects (all with force)",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            const bool force = params.value("force", false);
            bool hasError = false;
            const int count = doc->recompute({}, force, &hasError);
            Json errors = Json::array();
            for (auto* obj : doc->getObjects()) {
                if (obj->isError()) {
                    const char* message = doc->getErrorDescription(obj);
                    errors.push_back(
                        {{"object", obj->getNameInDocument()},
                         {"message", message ? message : obj->getStatusString()}}
                    );
                }
            }
            return Json {{"recomputed", count}, {"errors", errors}};
        }
    );

    server.registerCommand("Undo", "Undo the last transaction", [](const Json& params, RequestContext&) {
        App::Document* doc = requireDocument(params);
        doc->undo();
        return undoStack(doc);
    });

    server.registerCommand(
        "Redo",
        "Redo the last undone transaction",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            doc->redo();
            return undoStack(doc);
        }
    );

    server.registerCommand(
        "GetUndoStack",
        "Names of the undo and redo steps",
        [](const Json& params, RequestContext&) { return undoStack(requireDocument(params)); }
    );

    server.registerCommand(
        "OpenTransaction",
        "Start a named undo step; changes until CommitTransaction undo together",
        [](const Json& params, RequestContext&) {
            App::Document* doc = requireDocument(params);
            doc->openTransaction(requireString(params, "name"));
            return Json(nullptr);
        }
    );

    server.registerCommand(
        "CommitTransaction",
        "Close the open undo step",
        [](const Json& params, RequestContext&) {
            requireDocument(params)->commitTransaction();
            return Json(nullptr);
        }
    );

    server.registerCommand(
        "AbortTransaction",
        "Roll back and drop the open undo step",
        [](const Json& params, RequestContext&) {
            requireDocument(params)->abortTransaction();
            return Json(nullptr);
        }
    );
}

}  // namespace Api
