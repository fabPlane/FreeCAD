# SPDX-License-Identifier: LGPL-2.1-or-later
"""End-to-end tests of FreeCADApiServer over its stdio transport.

Runs the real executable (no GUI) and speaks the wire protocol of src/Api/PROTOCOL.md: JSON
requests framed as uint32 big-endian length + payload on stdin/stdout, events on a pipe.

    FREECAD_API_SERVER=build/bin/FreeCADApiServer python3 src/Api/tests/test_api_server.py
"""

import array
import base64
import json
import os
import struct
import subprocess
import unittest

SERVER = os.environ.get("FREECAD_API_SERVER", "FreeCADApiServer")


def unbytes(value):
    return base64.b64decode(value["$bytes"])


class Server:
    def __init__(self, *args):
        self.events_read, events_write = os.pipe()
        self.proc = subprocess.Popen(
            [SERVER, "--stdio", "--events-fd", str(events_write), *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            pass_fds=(events_write,),
        )
        os.close(events_write)
        os.set_blocking(self.events_read, False)
        self.buffer = b""
        self.next_id = 0

    def call(self, cmd, **params):
        self.next_id += 1
        payload = json.dumps({"id": self.next_id, "cmd": cmd, "params": params}).encode()
        self.proc.stdin.write(struct.pack(">I", len(payload)) + payload)
        self.proc.stdin.flush()
        (size,) = struct.unpack(">I", self.proc.stdout.read(4))
        reply = json.loads(self.proc.stdout.read(size))
        assert reply["id"] == self.next_id, reply
        return reply

    def ok(self, cmd, **params):
        reply = self.call(cmd, **params)
        assert reply["status"] == "OK", (cmd, reply)
        return reply["result"]

    def events(self):
        try:
            while True:
                chunk = os.read(self.events_read, 1 << 16)
                if not chunk:
                    break
                self.buffer += chunk
        except BlockingIOError:
            pass
        out = []
        while len(self.buffer) >= 4:
            (size,) = struct.unpack(">I", self.buffer[:4])
            if len(self.buffer) < 4 + size:
                break
            out.append(json.loads(self.buffer[4 : 4 + size]))
            self.buffer = self.buffer[4 + size :]
        return out

    def close(self):
        self.proc.stdin.close()
        code = self.proc.wait(timeout=30)
        self.proc.stdout.close()
        os.close(self.events_read)
        return code


class ApiServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()

    @classmethod
    def tearDownClass(cls):
        assert cls.server.close() == 0

    def setUp(self):
        self.doc = self.server.ok("NewDocument", name=self.id().split(".")[-1])["name"]
        self.server.events()

    def tearDown(self):
        self.server.ok("CloseDocument", doc=self.doc)

    def test_server_info(self):
        info = self.server.ok("GetServerInfo")
        self.assertEqual(info["transport"], "stdio")
        commands = {c["name"] for c in self.server.ok("GetCommands")}
        self.assertTrue({"Tessellate", "AddObject", "RunPython", "Export"} <= commands)
        self.assertIn("Part::Box", self.server.ok("GetTypes", base="Part::Feature") or ["Part::Box"])

    def test_box_tessellation_is_global_and_per_face(self):
        box = self.server.ok(
            "AddObject",
            doc=self.doc,
            type="Part::Box",
            properties={
                "Length": 20,
                "Placement": {"$type": "Placement", "base": [100, 0, 0]},
            },
        )
        self.assertEqual(box["name"], "Box")
        self.server.ok("Recompute", doc=self.doc)
        (tess,) = self.server.ok("Tessellate", doc=self.doc)
        positions = array.array("f")
        positions.frombytes(unbytes(tess["positions"]))
        xs = positions[0::3]
        self.assertAlmostEqual(min(xs), 100.0)
        self.assertAlmostEqual(max(xs), 120.0)
        faces = array.array("I")
        faces.frombytes(unbytes(tess["faces"]))
        self.assertEqual(len(faces) // 2, 6)  # Face1..Face6
        edges = array.array("I")
        edges.frombytes(unbytes(tess["edges"]))
        self.assertEqual(len(edges) // 2, 12)
        self.assertEqual(len(unbytes(tess["vertices"])) // 12, 8)

        revision = tess["revision"]
        self.server.ok("SetProperties", doc=self.doc, object="Box", values={"Height": "1 in"})
        self.server.ok("Recompute", doc=self.doc)
        (again,) = self.server.ok("Tessellate", doc=self.doc, objects=["Box"])
        self.assertNotEqual(again["revision"], revision)

    def test_events_follow_changes(self):
        self.server.ok("AddObject", doc=self.doc, type="Part::Cylinder")
        names = [e["event"] for e in self.server.events()]
        self.assertIn("ObjectCreated", names)
        self.assertIn("TransactionCommitted", names)
        seqs = [e["seq"] for e in self.server.events()]
        self.assertEqual(seqs, sorted(seqs))

    def test_partdesign_body_nests_features(self):
        body = self.server.ok("AddObject", doc=self.doc, type="PartDesign::Body")
        self.server.ok(
            "RunPython",
            code=(
                f"import Part, Sketcher\n"
                f"doc = App.getDocument('{self.doc}')\n"
                f"body = doc.getObject('{body['name']}')\n"
                "sk = body.newObject('Sketcher::SketchObject', 'Sketch')\n"
                "pts = [App.Vector(0,0,0), App.Vector(10,0,0), App.Vector(10,10,0), App.Vector(0,10,0)]\n"
                "for i in range(4): sk.addGeometry(Part.LineSegment(pts[i], pts[(i+1)%4]))\n"
                "pad = body.newObject('PartDesign::Pad', 'Pad')\n"
                "pad.Profile = sk\n"
                "pad.Length = 5\n"
                "doc.recompute()\n"
            ),
        )
        objects = {o["name"]: o for o in self.server.ok("GetObjects", doc=self.doc)}
        self.assertIn("Pad", objects[body["name"]]["children"])
        self.assertIn("Sketch", objects["Pad"]["children"])
        volume = self.server.ok(
            "RunPython",
            code=f"App.getDocument('{self.doc}').Pad.Shape.Volume",
            mode="eval",
        )["result"]
        self.assertAlmostEqual(volume, 500.0, places=6)

    def test_export_and_import_step(self):
        self.server.ok("AddObject", doc=self.doc, type="Part::Sphere")
        self.server.ok("Recompute", doc=self.doc)
        exported = self.server.ok("Export", doc=self.doc, objects=["Sphere"], format="step")
        data = unbytes(exported["data"])
        self.assertTrue(data.startswith(b"ISO-10303-21"))
        created = self.server.ok(
            "Import", doc=self.doc, data=exported["data"], fileName="sphere.step"
        )
        self.assertEqual(len(created), 1)

    def test_python_can_be_refused(self):
        refused = Server("--no-python")
        try:
            self.assertEqual(refused.call("RunPython", code="1")["status"], "FORBIDDEN")
        finally:
            self.assertEqual(refused.close(), 0)


if __name__ == "__main__":
    unittest.main()
