# FreeCAD API

A request/response API over FreeCAD's application core, so that programs outside FreeCAD — first
of all [fab_cad](https://github.com/fabPlane/fab_cad), the web frontend — can open documents,
create and edit objects, recompute, undo, and read tessellated geometry to draw. The wire format
is in [PROTOCOL.md](PROTOCOL.md).

It follows the design of the IPC API in fabPlane's KiCad fork (`fabPlane/kicad`, `api/`,
`common/api/`, `host/`): one envelope, one dispatcher, several transports, and a C ABI so the
same core can be compiled to WebAssembly.

| KiCad fork                                   | Here                                          |
| -------------------------------------------- | --------------------------------------------- |
| protobuf `ApiRequest` / `ApiResponse`        | JSON or CBOR `{id, cmd, params}` / `{id, status, result}` |
| `KICAD_API_SERVER` + `API_HANDLER_*`         | `Api::Server` + `Handlers*.cpp`               |
| nng `ipc://` and `ws://` sockets             | `WebSocketTransport` (Boost.Beast)            |
| `kicad-api-host-native` (stdio, fd 3 events) | `FreeCADApiServer --stdio` (same framing)     |
| `kiapi_*` C ABI, `kicad_api.wasm`            | `fcapi_*` C ABI (`ApiC.h`), `freecad_api.wasm` |
| `kicad-cli api-server`                       | `FreeCADApiServer`                            |
| events on a PUB socket                       | events pushed on the same connection / fd 3   |

Why JSON/CBOR and not protobuf: FreeCAD objects are bags of typed properties that modules add at
run time, and every property already converts itself to and from Python. Going through Python
(`Values.cpp`) covers every property type with one converter; a schema would have to be kept
in step with every module. CBOR keeps geometry binary; JSON keeps the API easy to poke at.

## Layout

| File                          | What                                                              |
| ----------------------------- | ----------------------------------------------------------------- |
| `Server.*`                    | Dispatcher: command table, envelope, status codes, events          |
| `Codec.*`                     | JSON / CBOR encoding, `$bytes`                                     |
| `Values.*`                    | Python ⇄ JSON tagged values, `PropertyInfo`                        |
| `Handlers*.cpp`               | The commands, grouped: server, documents, objects, geometry, script |
| `Transaction.h`               | Each editing command is one undo step unless a transaction is open |
| `Executor.*`                  | Runs requests on the main thread (Qt loop, or a plain queue)       |
| `WebSocketTransport.*`        | `ws://` server, origin check, optional key                         |
| `StdioTransport.*`            | Length-prefixed frames on stdin/stdout, events on fd 3             |
| `ApiC.*`, `Host.*`            | The C ABI and FreeCAD start-up for in-process hosts and wasm       |
| `AppApiPy.cpp`                | The `FreeCADApi` Python module                                     |
| `../Main/MainApi.cpp`         | `FreeCADApiServer`                                                 |
| `tests/test_api_server.py`, `../../tests/src/Api/` | End-to-end (stdio) and unit (gtest) tests    |

## Build and run

`BUILD_API` is ON by default. It needs nothing FreeCAD does not already use (Boost.Beast is
header-only; nlohmann/json is bundled). A headless build is enough:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
ninja -C build FreeCADApiServer FreeCADApi

build/bin/FreeCADApiServer --listen ws://127.0.0.1:8765/     # prints: FCAPI_READY ws://127.0.0.1:8765/
build/bin/FreeCADApiServer --stdio                           # for a supervising process
build/bin/FreeCADApiServer --listen ws://127.0.0.1:0/ model.FCStd   # any free port, preload a file
```

From a running FreeCAD (desktop or `FreeCADCmd`):

```python
import FreeCADApi
FreeCADApi.startServer("ws://127.0.0.1:8765/")   # desktop: requests run in the GUI's event loop
FreeCADApi.serve("ws://127.0.0.1:8765/")         # FreeCADCmd: blocks until stopServer()
```

With the desktop FreeCAD serving, the web frontend edits the same document the user sees.

## Security

The API can run Python, so it is as powerful as the user running FreeCAD.

- The WebSocket binds `127.0.0.1` unless told otherwise.
- Browser pages are refused unless their `Origin` is allowed (`--allow-origin`; default
  `http://localhost` and `http://127.0.0.1` on any port). This stops a web site from driving a
  local FreeCAD through the user's browser. Non-browser clients send no `Origin`.
- `--key KEY` requires `?key=KEY` on the connection URL.
- `--no-python` refuses `RunPython` (the rest of the API cannot run arbitrary code, but
  `Import` still runs FreeCAD's importers on data the client sends).

## Tests

```sh
cmake -B build -DENABLE_DEVELOPER_TESTS=ON && ninja -C build Api_tests_run
build/tests/src/Api/Api_tests_run                           # codec + dispatcher (gtest)
FREECAD_API_SERVER=build/bin/FreeCADApiServer python3 src/Api/tests/test_api_server.py
```

## Adding a command

Register it in the matching `Handlers*.cpp` with a one-line description (it is what
`GetCommands` reports), throw `ApiError` for anything but a FreeCAD failure, wrap edits in an
`AutoTransaction`, add it to PROTOCOL.md, and cover it in a test. Keep names CamelCase verbs.
