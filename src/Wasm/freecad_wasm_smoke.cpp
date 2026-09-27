// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD project association AISBL               *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

// freecad_wasm_smoke: proves FreeCAD's application core runs in WebAssembly.
//
//     node freecad_wasm_smoke.js
//
// Initialises App::Application, creates a document, adds a Part::Box through
// the C++ API, recomputes, and checks the volume - then does the same through
// the embedded Python interpreter (import Part, Sketcher, PartDesign), which
// exercises the built-in module registration.  Exit code 0 = every check
// passed.

#include <FCConfig.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <App/Application.h>
#include <App/Document.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/TopoShape.h>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include "FreeCADWasm.h"

namespace
{
int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int /*argc*/, char** /*argv*/)
{
    const auto t0 = std::chrono::steady_clock::now();

    FreeCADWasm_setupEnvironment();
    FreeCADWasm_registerBuiltinModules();

    App::Application::Config()["ExeName"] = "FreeCAD";
    App::Application::Config()["ExeVendor"] = "FreeCAD";
    App::Application::Config()["AppDataSkipVendor"] = "true";
    App::Application::Config()["RunMode"] = "Exit";
    App::Application::Config()["Console"] = "1";
    App::Application::Config()["LoggingConsole"] = "1";

    static char arg0[] = "FreeCAD";
    static char arg1[] = "--console";
    static char* args[] = {arg0, arg1, nullptr};

    try {
        App::Application::init(2, args);
    }
    catch (const Base::Exception& e) {
        std::printf("FAIL App::Application::init: %s\n", e.what());
        std::printf("python path: %s\n", Base::Interpreter().getPythonPath().c_str());
        return 2;
    }
    catch (const std::exception& e) {
        std::printf("FAIL App::Application::init: %s\n", e.what());
        return 2;
    }
    std::printf("init: %.0f ms (FreeCAD %s.%s.%s, home %s)\n",
                msSince(t0),
                App::Application::Config()["BuildVersionMajor"].c_str(),
                App::Application::Config()["BuildVersionMinor"].c_str(),
                App::Application::Config()["BuildVersionPoint"].c_str(),
                App::Application::getHomePath().c_str());

    try {
        // --- C++ API ---------------------------------------------------------
        // Part::Box is registered with the type system by PyInit_Part, i.e.
        // by importing the (built-in) module.
        Base::Interpreter().runString("import Part");

        const auto t1 = std::chrono::steady_clock::now();
        App::Document* doc = App::GetApplication().newDocument("Smoke");
        check(doc != nullptr, "newDocument");

        auto* box = dynamic_cast<Part::Box*>(doc->addObject("Part::Box", "Box"));
        check(box != nullptr, "addObject(Part::Box)");
        box->Length.setValue(10.0);
        box->Width.setValue(20.0);
        box->Height.setValue(30.0);
        const int recomputed = doc->recompute();
        check(recomputed > 0, "recompute (" + std::to_string(recomputed) + " object(s))");
        check(box->isValid(), "Box is valid");

        GProp_GProps props;
        BRepGProp::VolumeProperties(box->Shape.getShape().getShape(), props);
        const double volume = props.Mass();
        std::printf("Part::Box 10x20x30 volume = %.6f (C++, %.1f ms)\n", volume, msSince(t1));
        check(std::fabs(volume - 6000.0) < 1e-6, "volume == 6000");

        // --- Python ----------------------------------------------------------
        const auto t2 = std::chrono::steady_clock::now();
        std::string pyVolume = Base::Interpreter().runStringWithKey(
            "import FreeCAD, Part\n"
            "d = FreeCAD.getDocument('Smoke')\n"
            "c = d.addObject('Part::Cylinder', 'Cyl')\n"
            "c.Radius = 5\n"
            "c.Height = 10\n"
            "cut = d.addObject('Part::Cut', 'Cut')\n"
            "cut.Base = d.Box\n"
            "cut.Tool = c\n"
            "d.recompute()\n"
            "_ = '%.6f' % cut.Shape.Volume\n",
            "_");
        std::printf("Box - Cylinder(r=5,h=10) volume = %s (Python, %.1f ms)\n",
                    pyVolume.c_str(), msSince(t2));
        const double expected = 6000.0 - M_PI * 25.0 * 10.0 / 4.0;  // quarter cylinder at the corner
        check(std::fabs(std::stod(pyVolume) - expected) < 1e-3,
              "boolean cut volume == 6000 - 62.5*pi");

        std::string mods = Base::Interpreter().runStringWithKey(
            // exec() with separate locals: no comprehensions over locals
            "import sys, Sketcher, PartDesign, Materials\n"
            "found = []\n"
            "for m in ('Part', 'Sketcher', '_PartDesign', 'Materials'):\n"
            "    if m in sys.builtin_module_names:\n"
            "        found.append(m)\n"
            "_ = ' '.join(found)\n",
            "_");
        std::printf("built-in FreeCAD modules: %s\n", mods.c_str());
        check(mods == "Part Sketcher _PartDesign Materials", "modules are built-ins");

        std::string sketch = Base::Interpreter().runStringWithKey(
            "import FreeCAD, Part, Sketcher\n"
            "from FreeCAD import Vector as V\n"
            "d = FreeCAD.getDocument('Smoke')\n"
            "body = d.addObject('PartDesign::Body', 'Body')\n"
            "sk = body.newObject('Sketcher::SketchObject', 'Sketch')\n"
            "pts = [V(0,0,0), V(10,0,0), V(10,10,0), V(0,10,0)]\n"
            "for i in range(4):\n"
            "    sk.addGeometry(Part.LineSegment(pts[i], pts[(i+1)%4]))\n"
            "for i in range(4):\n"
            "    sk.addConstraint(Sketcher.Constraint('Coincident', i, 2, (i+1)%4, 1))\n"
            "pad = body.newObject('PartDesign::Pad', 'Pad')\n"
            "pad.Profile = sk\n"
            "pad.Length = 5\n"
            "d.recompute()\n"
            "_ = '%.6f' % pad.Shape.Volume\n",
            "_");
        std::printf("PartDesign Pad of a 10x10 sketch, 5 high: volume = %s\n", sketch.c_str());
        check(std::fabs(std::stod(sketch) - 500.0) < 1e-6, "pad volume == 500");

        App::GetApplication().closeDocument("Smoke");
    }
    catch (const Base::Exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        ++failures;
    }
    catch (const std::exception& e) {
        std::printf("FAIL std::exception: %s\n", e.what());
        ++failures;
    }

    std::printf("total: %.0f ms, %d failure(s)\n", msSince(t0), failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
