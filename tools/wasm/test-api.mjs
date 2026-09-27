#!/usr/bin/env node
// Drive freecad_api.{js,wasm,data} through its C ABI, the way a browser
// worker would: JSON requests into fcapi_dispatch, JSON replies out.
//
//   node tools/wasm/test-api.mjs [<dir containing freecad_api.js>]
//
// Default directory: $FREECAD_WASM_BUILD/wasm (or /home/user/wasm-build/freecad/wasm).
// Exit code 0 = every check passed.

import path from "node:path";
import { pathToFileURL } from "node:url";

const dir = path.resolve(
  process.argv[2] ?? path.join(process.env.FREECAD_WASM_BUILD ?? "/home/user/wasm-build/freecad", "wasm"),
);
const { default: createFreecadApi } = await import(pathToFileURL(path.join(dir, "freecad_api.js")).href);

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failures++;
};

const t0 = performance.now();
const events = [];
const m = await createFreecadApi({
  locateFile: (f) => path.join(dir, f),
  print: (t) => console.log(`[freecad] ${t}`),
  printErr: (t) => console.log(`[freecad:err] ${t}`),
});
m.__fcapiEvent = (bytes) => events.push(new TextDecoder().decode(bytes.slice()));
const tLoad = performance.now() - t0;

const cfg = m.stringToNewUTF8(JSON.stringify({ eventEncoding: "json", modules: ["Part", "PartDesign"] }));
const tInit0 = performance.now();
const rc = m._fcapi_init(cfg);
m._free(cfg);
const tInit = performance.now() - tInit0;
check(rc === 0, `fcapi_init -> ${rc}${rc ? ": " + m.UTF8ToString(m._fcapi_last_error()) : ""}`);
if (rc !== 0) process.exit(1);

let nextId = 1;
function call(cmd, params = {}) {
  const req = new TextEncoder().encode(JSON.stringify({ id: nextId++, cmd, params }));
  const reqPtr = m._malloc(req.length);
  m.HEAPU8.set(req, reqPtr);
  const outLen = m._malloc(4);
  const replyPtr = m._fcapi_dispatch(reqPtr, req.length, outLen);
  m._free(reqPtr);
  if (!replyPtr) {
    m._free(outLen);
    throw new Error(`${cmd}: ${m.UTF8ToString(m._fcapi_last_error())}`);
  }
  const n = new Uint32Array(m.HEAPU8.buffer, outLen, 1)[0];
  const text = new TextDecoder().decode(m.HEAPU8.slice(replyPtr, replyPtr + n));
  m._fcapi_free(replyPtr);
  m._free(outLen);
  return JSON.parse(text);
}

const timed = (cmd, params) => {
  const t = performance.now();
  const r = call(cmd, params);
  return [r, performance.now() - t];
};

let r = call("Ping");
check(r.status === "OK", "Ping");
const pingT = performance.now();
for (let i = 0; i < 200; i++) call("Ping");
const pingMs = (performance.now() - pingT) / 200;

r = call("GetVersion");
check(r.status === "OK", `GetVersion -> ${r.result?.full}`);

r = call("GetServerInfo");
check(r.status === "OK" && r.result.transport === "inproc", `GetServerInfo -> ${r.result?.url} platform=${r.result?.platform}`);

r = call("NewDocument", { name: "Api" });
check(r.status === "OK", "NewDocument");
const doc = r.result?.name ?? "Api";

r = call("AddObject", { doc, type: "Part::Box", name: "Box", properties: { Length: 10, Width: 20, Height: 30 } });
check(r.status === "OK", `AddObject Part::Box ${r.error ?? ""}`);

let recomputeMs;
[r, recomputeMs] = timed("Recompute", { doc });
check(r.status === "OK" && r.result.errors.length === 0, `Recompute -> ${JSON.stringify(r.result ?? r.error)}`);

r = call("GetBoundingBox", { doc });
check(r.status === "OK", `GetBoundingBox -> ${JSON.stringify(r.result ?? r.error)}`);

let tessMs;
[r, tessMs] = timed("Tessellate", { doc, deflection: 0.1 });
check(r.status === "OK" && r.result.length === 1, `Tessellate -> ${r.status} ${r.error ?? ""}`);

r = call("RunPython", { code: "import FreeCAD\n_ = FreeCAD.getDocument('Api').Box.Shape.Volume" });
check(r.status === "OK", `RunPython -> ${JSON.stringify(r.result ?? r.error).slice(0, 120)}`);

r = call("SaveDocumentBytes", { doc });
check(r.status === "OK", `SaveDocumentBytes -> ${r.status} ${r.error ?? ""}`);

r = call("NoSuchCommand");
check(r.status === "UNKNOWN_COMMAND", "unknown command is UNKNOWN_COMMAND");

check(events.length > 0, `events delivered: ${events.length} (first: ${events[0]?.slice(0, 80)})`);

r = call("CloseDocument", { doc });
check(r.status === "OK", "CloseDocument");
m._fcapi_shutdown();

console.log(
  `module load ${tLoad.toFixed(0)} ms, fcapi_init ${tInit.toFixed(0)} ms, ` +
    `Ping ${pingMs.toFixed(3)} ms, Recompute ${recomputeMs.toFixed(1)} ms, Tessellate ${tessMs.toFixed(1)} ms`,
);
console.log(`${failures} failure(s)`);
process.exit(failures ? 1 : 0);
