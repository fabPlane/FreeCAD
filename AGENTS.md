# Working in this repository

This is fabPlane's fork of FreeCAD. It carries the FreeCAD API (`src/Api`) that
[fab_cad](https://github.com/fabPlane/fab_cad), the web frontend, is built on, and the
WebAssembly build of the application core (`tools/wasm`). The design mirrors fabPlane's KiCad
fork (`fabPlane/kicad`: its IPC API, `host/` and `tools/wasm/`) and its web frontend `fab_pcb`.

## Branches

- **`main` is the only long-lived branch.** Open branches against `main` and target `main`
  with every pull request.
- Merge with a merge commit, never rebase or force-push a branch someone else may have checked out.
- Upstream FreeCAD is merged into `main` by a sync pull request (`upstream-sync-<date>` → `main`);
  never push `main` directly.
- A fab_cad change set records the fork commit it was built against in
  `packages/protocol/FREECAD_COMMIT` over there.

## The API

Read `src/Api/README.md` and `src/Api/PROTOCOL.md` before changing anything under `src/Api`.
A new or changed command comes with its PROTOCOL.md entry and a test (`tests/src/Api` for the
dispatcher, `src/Api/tests/test_api_server.py` end to end). Keep changes outside `src/Api`
small, guarded and upstreamable.

```sh
cmake -S . -B build -G Ninja -DBUILD_GUI=OFF -DENABLE_DEVELOPER_TESTS=ON   # see .github/workflows/fabplane_api.yml
ninja -C build FreeCADApiServer FreeCADApi Api_tests_run
build/tests/src/Api/Api_tests_run
FREECAD_API_SERVER=build/bin/FreeCADApiServer python3 src/Api/tests/test_api_server.py
```

## Code

Follow FreeCAD's style (`.clang-format`, needs clang-format 20 or newer; `pip install
clang-format`). MSVC is a supported compiler: avoid GCC-only idioms and mark exported classes
with `ApiExport`.
