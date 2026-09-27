// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Host.h"

#include <clocale>
#include <cstring>
#include <format>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/Version.h>
#include <App/Application.h>
#include <App/ProgramInformation.h>

namespace Api
{

namespace
{
bool initialized = false;
}

bool freeCADInitialized()
{
    return initialized;
}

bool initializeFreeCAD(int argc, char** argv, std::string& error)
{
    if (initialized) {
        return true;
    }

    std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");

    auto& config = App::Application::Config();
    config["ExeName"] = "FreeCAD";
    config["ExeVendor"] = "FreeCAD";
    config["AppDataSkipVendor"] = "true";
    config["CopyrightInfo"] = std::format(
        "(C) 2001-{} FreeCAD contributors\n"
        "FreeCAD is free and open-source software licensed under the terms of LGPL2+ license.\n\n",
        Base::FCVersionInfo::CopyrightYear()
    );
    config["RunMode"] = "Exit";
    config["LoggingConsole"] = "1";
    // No splash text on stdout: the stdio transport owns it.
    config["Verbose"] = "Strict";

    try {
        App::Application::init(argc, argv);
    }
    catch (const Base::ProgramInformation& e) {
        error = e.what();
        return false;
    }
    catch (const Base::Exception& e) {
        error = std::string("FreeCAD failed to start: ") + e.what()
            + "\nPython path: " + Base::Interpreter().getPythonPath();
        return false;
    }
    catch (const std::exception& e) {
        error = std::string("FreeCAD failed to start: ") + e.what();
        return false;
    }
    catch (...) {
        error = "FreeCAD failed to start";
        return false;
    }

    initialized = true;
    return true;
}

void shutdownFreeCAD()
{
    if (!initialized) {
        return;
    }
    try {
        App::GetApplication().closeAllDocuments();
    }
    catch (...) {
    }
    App::Application::destruct();
    initialized = false;
}

}  // namespace Api
