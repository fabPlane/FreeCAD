# FreeCAD API — wire protocol (version 1)

The FreeCAD API lets a program outside FreeCAD drive the application core: open and save
documents, add, change and remove objects, recompute, undo, and read back tessellated geometry
to draw. It is what [fab_cad](https://github.com/fabPlane/fab_cad) — the web frontend — talks to.
It mirrors the design of the IPC API in fabPlane's KiCad fork (a request/response envelope, one
dispatcher, several transports), adapted to FreeCAD's dynamic property model.

## One dispatcher, three transports

```
  FreeCADApiServer --listen ws://127.0.0.1:8765     (WebSocket; browsers dial it directly)
  FreeCADApiServer --stdio                          (length-prefixed pipes; tests, supervisors)
  freecad_api.wasm  fcapi_dispatch(bytes)           (in-process; WebAssembly in a page or a worker)
                 \            |             /
                  Api::Server::dispatch(bytes) -> bytes
                              |
                      App::Application (FreeCAD's core, no GUI)
```

Every transport carries the same messages. The dispatcher does not know which one it serves.
`import FreeCADApi; FreeCADApi.startServer("ws://127.0.0.1:8765")` starts the WebSocket
transport inside a running FreeCAD (GUI or not), so the web frontend can also attach to a
desktop session.

## Encoding

A message is a JSON object, sent either as **JSON text** or as **CBOR** (RFC 8949). The server
answers in the encoding it was asked in. The first byte tells them apart: `{` (0x7B) is JSON,
anything else is CBOR (a CBOR map starts with 0xA0–0xBF).

- On WebSocket, a text frame is JSON, a binary frame is CBOR.
- Binary data (tessellations, file contents) is a CBOR byte string in CBOR, and
  `{"$bytes": "<base64>"}` in JSON. Clients should prefer CBOR for geometry.

## Request

```json
{ "id": 7, "cmd": "GetObjects", "params": { "doc": "Unnamed" }, "token": "", "client": "fab-cad/tab-1" }
```

| Field    | Type   | Meaning                                                                               |
| -------- | ------ | ------------------------------------------------------------------------------------- |
| `id`     | number | Echoed in the reply. Any value; replies may be matched by it.                         |
| `cmd`    | string | Command name (see `GetCommands`).                                                     |
| `params` | object | Command parameters. Optional; defaults to `{}`.                                       |
| `token`  | string | Optional. The server's instance token from an earlier reply; a mismatch is refused. |
| `client` | string | Optional. Free-form client name, reported in events the request caused.               |

## Response

```json
{ "id": 7, "status": "OK", "token": "5f0c…", "result": { … } }
{ "id": 8, "status": "NOT_FOUND", "token": "5f0c…", "error": "No object 'Box001' in document 'Unnamed'" }
```

| `status`          | Meaning                                                                   |
| ----------------- | ------------------------------------------------------------------------- |
| `OK`              | Done; `result` holds the answer (may be `null`).                          |
| `BAD_REQUEST`     | The envelope or a parameter is malformed or missing.                      |
| `UNKNOWN_COMMAND` | No handler for `cmd`.                                                     |
| `NOT_FOUND`       | A named document, object or property does not exist.                      |
| `TOKEN_MISMATCH`  | The request's `token` is not this server's (it restarted; reload).        |
| `FORBIDDEN`       | The command is disabled on this server (e.g. `RunPython` with `--no-python`). |
| `FAILED`          | FreeCAD raised an error; `error` is its message.                          |

`token` is a random string fixed for the life of the process (the `kicad_token` of the KiCad
API). Requests are executed **one at a time, in arrival order, on FreeCAD's main thread**.

## Events

```json
{ "event": "ObjectChanged", "seq": 42, "data": { "doc": "Unnamed", "object": "Box", "property": "Length" } }
```

Events are pushed on the same WebSocket connection (a message with `event` and no `id`), on fd 3
for the stdio host, and through the event callback for the C ABI. Events raised while a request
runs are sent **after** that request's reply. `seq` increases by one per event, per server.
`data.client` names the client whose request caused it, when there was one.

| Event                  | `data`                                         |
| ---------------------- | ---------------------------------------------- |
| `DocumentCreated`      | `doc`, `label`                                 |
| `DocumentDeleted`      | `doc`                                          |
| `DocumentRenamed`      | `doc`, `label`                                 |
| `ActiveDocumentChanged`| `doc`                                          |
| `DocumentSaved`        | `doc`, `fileName`                              |
| `DocumentRestored`     | `doc`                                          |
| `ObjectCreated`        | `doc`, `object`, `type`                        |
| `ObjectDeleted`        | `doc`, `object`                                |
| `ObjectChanged`        | `doc`, `object`, `property`                    |
| `ObjectRecomputed`     | `doc`, `object`                                |
| `Recomputed`           | `doc`                                          |
| `TransactionOpened`    | `doc`, `name`                                  |
| `TransactionCommitted` | `doc`                                          |
| `TransactionAborted`   | `doc`                                          |
| `Undo` / `Redo`        | `doc`                                          |

`ObjectChanged` is coalesced per request: one event per (object, property) pair.

## Values

Property values travel as JSON with a few tagged forms for FreeCAD types. The same form is
accepted by `SetProperties`, and `AddObject`'s `properties`.

| FreeCAD                                  | JSON                                                              |
| ---------------------------------------- | ----------------------------------------------------------------- |
| bool, int, float, str                    | as is                                                             |
| list / tuple                             | array                                                             |
| dict                                     | object                                                            |
| `Base.Vector`                            | `{"$type":"Vector","x":0,"y":0,"z":0}`                            |
| `Base.Rotation`                          | `{"$type":"Rotation","q":[x,y,z,w],"axis":[..],"angle":deg}`      |
| `Base.Placement`                         | `{"$type":"Placement","base":[x,y,z],"rotation":[x,y,z,w]}`       |
| `Base.Matrix`                            | `{"$type":"Matrix","a":[16 numbers, row major]}`                  |
| `Base.Quantity` / `Units.Quantity`       | `{"$type":"Quantity","value":10,"unit":"mm","text":"10 mm"}`      |
| a document object                        | `{"$type":"Object","doc":"Unnamed","name":"Box"}`                 |
| a link with sub-elements                 | `[{"$type":"Object",…}, ["Face1","Edge2"]]` (as Python gives it)  |
| bytes                                    | CBOR bytes / `{"$bytes":"…"}`                                     |
| anything else                            | `{"$type":"Repr","type":"<python type>","repr":"…"}` (read only) |

A `Rotation` may be sent as `q` alone, or `axis` + `angle` (degrees). A `Quantity` may be sent
as a plain number (in the property's unit) or as a string FreeCAD parses (`"25.4 mm"`, `"1 in"`).

## Commands

Names are CamelCase verbs, like KiCad's API. `doc` is a document's internal name; `object` is an
object's internal name (not its label).

### Server

| Command         | Params                     | Result                                                                  |
| --------------- | -------------------------- | ----------------------------------------------------------------------- |
| `Ping`          | —                          | `null`                                                                  |
| `GetVersion`    | —                          | `{major, minor, patch, revision, full, api}` (`api` = protocol version) |
| `GetServerInfo` | —                          | `{token, transport, url, python, events, pid, platform, homePath, userDataPath}` |
| `GetCommands`   | —                          | `[{name, description}]`                                                 |
| `GetTypes`      | `{base?}`                  | `[type]` — registered types derived from `base` (default `App::DocumentObject`) |
| `LoadModule`    | `{name}`                   | `null` — import a FreeCAD module (`Part`, `PartDesign`, `Sketcher`, …)  |

### Documents

| Command               | Params                        | Result                                   |
| --------------------- | ----------------------------- | ---------------------------------------- |
| `ListDocuments`       | —                             | `[Document]`                             |
| `NewDocument`         | `{name?, label?}`             | `Document`                               |
| `OpenDocument`        | `{path}`                      | `Document`                               |
| `OpenDocumentBytes`   | `{data, fileName?}`           | `Document` — from `.FCStd` bytes         |
| `SaveDocument`        | `{doc}`                       | `Document`                               |
| `SaveDocumentAs`      | `{doc, path}`                 | `Document`                               |
| `SaveDocumentBytes`   | `{doc}`                       | `{data, fileName}` — the `.FCStd` bytes  |
| `CloseDocument`       | `{doc}`                       | `null`                                   |
| `SetActiveDocument`   | `{doc}`                       | `null`                                   |
| `Recompute`           | `{doc, force?}`               | `{recomputed, errors: [{object, message}]}` |
| `Undo` / `Redo`       | `{doc}`                       | `UndoStack`                              |
| `GetUndoStack`        | `{doc}`                       | `UndoStack` = `{undo: [name], redo: [name]}` (most recent first) |
| `OpenTransaction`     | `{doc, name}`                 | `null`                                   |
| `CommitTransaction`   | `{doc}`                       | `null`                                   |
| `AbortTransaction`    | `{doc}`                       | `null`                                   |

`Document` = `{name, label, fileName, modified, active, transient, objectCount, undoCount, redoCount}`.

### Objects

| Command          | Params                                            | Result                             |
| ---------------- | ------------------------------------------------- | ---------------------------------- |
| `GetObjects`     | `{doc}`                                           | `[ObjectInfo]` in document order   |
| `GetObject`      | `{doc, object, properties?}`                      | `ObjectInfo` (+ `properties`)      |
| `GetProperties`  | `{doc, object, names?}`                           | `[PropertyInfo]`                   |
| `SetProperties`  | `{doc, object, values: {name: value}}`            | `[PropertyInfo]` of those changed  |
| `SetExpression`  | `{doc, object, path, expression}` (`null` clears) | `null`                             |
| `AddObject`      | `{doc, type, name?, label?, properties?, group?}` | `ObjectInfo`                       |
| `RemoveObject`   | `{doc, object, recursive?}`                       | `null`                             |
| `AddProperty`    | `{doc, object, type, name, group?, doc?}`         | `PropertyInfo`                     |
| `RemoveProperty` | `{doc, object, name}`                             | `null`                             |

`ObjectInfo` = `{name, label, type, typeHierarchy, isGeo, isValid, isTouched, isError, status,
inList, outList, children, parents, visibility, bbox?}`. `children` is the object's claimed
children (what the tree view nests under it: group members, a sketch's parent body, …).
`bbox` is `{min:[x,y,z], max:[x,y,z]}` for objects with geometry.

`PropertyInfo` = `{name, type, group, doc, status: [flags], value, expression?, enum?, unit?}`
— `status` holds the property's flags (`ReadOnly`, `Hidden`, `Output`, `Transient`, `NoRecompute`,
`Dynamic`); `enum` the choices of an enumeration; `unit` the unit of a quantity.

### Geometry

| Command          | Params                                                          | Result            |
| ---------------- | --------------------------------------------------------------- | ----------------- |
| `Tessellate`     | `{doc, objects?, deflection?, edges?}`                          | `[Tessellation]`  |
| `GetBoundingBox` | `{doc, objects?}`                                               | `{min, max}`      |

`Tessellation` = `{object, placement, revision, deflection, positions, indices, faces, edges,
edgePositions, vertices}`:

- `positions` — float32 x,y,z triples (bytes, little-endian), in the **global** frame;
- `indices` — uint32 triangle indices (bytes); `faces` — per sub-face `[firstTriangle,
  triangleCount]`, so face `i` is `Face{i+1}` and can be picked;
- `edgePositions` — float32 polyline points (bytes); `edges` — per edge `[firstPoint, pointCount]`
  (`Edge{i+1}`); `vertices` — float32 points (bytes) (`Vertex{i+1}`);
- `revision` — changes whenever the object's shape changes, so a client can cache.

`deflection` defaults to 0.1% of the bounding box diagonal. Normals are not sent: every face
has its own vertices, so a client gets correct smooth shading by averaging within a face.
`edges: false` leaves edges and vertices out. Objects without a shape are left out.

### Import / export / scripting

| Command     | Params                                         | Result                                   |
| ----------- | ---------------------------------------------- | ---------------------------------------- |
| `Import`    | `{doc, path? , data?, fileName}`               | `[object]` created                       |
| `Export`    | `{doc, objects, format}` (`step`, `iges`, `brep`, `stl`, `obj`) | `{data, fileName}` |
| `RunPython` | `{code, mode?: "exec"\|"eval"\|"auto"}`       | `{stdout, stderr, result?, repr?, exception?}` |

`RunPython` runs in FreeCAD's `__main__` namespace, so `App`, `FreeCAD` and anything imported
before are there. It is how the frontend drives the workbenches whose commands are Python
scripts. `auto` evaluates an expression and falls back to executing statements, like the Python
console; `result` and `repr` are an expression's value. A Python exception comes back in
`exception` with status `OK` — the request itself was served. Start the server with
`--no-python` to refuse the command (`FORBIDDEN`).
