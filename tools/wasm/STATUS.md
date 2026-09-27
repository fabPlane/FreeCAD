# FreeCAD in WebAssembly — status, 2026-09-27

**The application core builds, links, loads and computes in WebAssembly.** Base, App, Materials,
Part, Sketcher, PartDesign and the API server's C ABI are one static wasm32 module. Under node
22.22: `freecad_wasm_smoke` passes **9/9 checks in 0.78 s** (App init 0.26 s, a `Part::Box` from
C++, a boolean cut and a PartDesign Body → Sketch → Pad from the embedded Python, a STEP/IGES round
trip and an STL export in MEMFS), and `freecad_api` passes **14/14 checks** through the `fcapi_*`
ABI (`fcapi_init` 0.28–0.34 s, `Ping` **0.010 ms**, a Box recompute 3 ms, a tessellation 16–22 ms,
16 events delivered through `Module.__fcapiEvent`). Everything is on branch `wasm-wip` of the
worktree `/home/user/freecad-wasm`; nothing is pushed.

How to build and run it: [README.md](README.md).

## 1. What runs (verbatim)

```
$ node freecad_wasm_smoke.js
init: 263 ms (FreeCAD 26.3.0, home /freecad//)
ok   newDocument
ok   addObject(Part::Box)
ok   recompute (1 object(s))
ok   Box is valid
Part::Box 10x20x30 volume = 6000.000000 (C++, 199.0 ms)
ok   volume == 6000
Box - Cylinder(r=5,h=10) volume = 5803.650459 (Python, 122.3 ms)
ok   boolean cut volume == 6000 - 62.5*pi
built-in FreeCAD modules: Part Sketcher _PartDesign Materials
ok   modules are built-ins
PartDesign Pad of a 10x10 sketch, 5 high: volume = 500.000000
ok   pad volume == 500
STEP volume, IGES area, STL bytes: 6000.000 2200.000 3052
ok   STEP/IGES round trip, STL export
total: 775 ms, 0 failure(s)
```

```
$ node tools/wasm/test-api.mjs /home/user/wasm-build/freecad/wasm
ok   fcapi_init -> 0
ok   Ping
ok   GetVersion -> 26.3.0dev
ok   GetServerInfo -> inproc://freecad platform=wasm
ok   NewDocument
ok   AddObject Part::Box
ok   Recompute -> {"errors":[],"recomputed":1}
ok   GetBoundingBox -> {"max":[10,20,30],"min":[0,0,0]}
ok   Tessellate -> OK
ok   RunPython -> {"stderr":"","stdout":""}
ok   SaveDocumentBytes -> OK
ok   unknown command is UNKNOWN_COMMAND
ok   events delivered: 16 (first: {"data":{"doc":"Api","label":"Unnamed"},"event":"DocumentCreated","seq":1})
ok   CloseDocument
module load 263 ms, fcapi_init 342 ms, Ping 0.015 ms, Recompute 2.9 ms, Tessellate 16.4 ms
0 failure(s)
```

Not tested: a browser (only node 22 is on this machine; the module is built
`ENVIRONMENT=node,web,worker` and uses nothing node-specific), and node 20.

## 2. Versions

| | version | |
| --- | --- | --- |
| Emscripten | **6.0.9** | same as the KiCad fork's module; `-O2 -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1` everywhere, no pthreads |
| CPython | **3.11.15** | = the native build's; static, stdlib as a 4.0 MB `.pyc` zip |
| Qt | **6.4.2** qtbase (Core, Xml) | = the host Qt whose moc/rcc run; Qt recommends emsdk 3.1.14 for 6.4 (a warning only) |
| OpenCASCADE | **7.6.3** | = the native build's; patched (§3c) |
| Boost | 1.86.0 | |
| ICU | 74.2 | = Ubuntu 24.04's, so the host `icupkg` matches |
| Xerces-C | 3.2.5 | ICU transcoder |
| FreeType / HarfBuzz | 2.13.3 / 8.3.0 | required by Part (§3f) |
| zlib, fmt, yaml-cpp, Eigen | 1.3.1, 11.0.2, 0.8.0, 3.4.0 | |

## 3. What it took — every non-obvious fix

a. **Static modules as Python built-ins** (the design, `cMake/FreeCAD_Helpers/SetupWasm.cmake`,
   `src/Wasm/`). The global `TARGET_SUPPORTS_SHARED_LIBS=FALSE` turns all 6 `add_library(SHARED)`
   in the build into static archives — no module CMakeLists changed. A generated
   `FreeCADWasm_registerBuiltinModules()` calls `PyImport_AppendInittab` for `Part`, `Sketcher`,
   `_PartDesign`, `Materials` before `Py_Initialize`; `sys.builtin_module_names` confirms it.

b. **No threads** (`FC_NO_THREADS`, from `FCConfig.h` for Emscripten without `-pthread`).
   `App::Application`'s constructor starts a recompute worker with `std::thread`, which throws
   without pthreads. The worker is not started and `queueRecomputeRequest` runs every request
   inline — the path headless FreeCAD already takes for not-worker-safe requests.

c. **LLVM emits invalid wasm for `setjmp` + legacy wasm EH in one function.** The first full link
   was rejected by both binaryen (`popping from empty stack`) and V8 (`Compiling function
   #24833:"ShapeUpgrade_ShapeDivide::Perform(bool)" failed: br_table: label arity inconsistent`).
   OCCT's `OCC_CONVERT_SIGNALS` puts a `setjmp` into every `OCC_CATCH_SIGNALS`, and those
   functions also `try`/`catch`. Signals never reach wasm code, so the conversion is useless
   there: `patches/occt-7.6.3-emscripten-no-signal-conversion.patch` drops it (and OCCT's
   `-fexceptions`) under Emscripten, which also removes `-DOCC_CONVERT_SIGNALS` from FreeCAD's
   compile lines (it came in through OCCT's CMake package). Cost: one OCCT rebuild (31 min).

d. **Qt makes every executable its app.** A wasm Qt's `Qt6::Platform` carries
   `-sMODULARIZE=1 -sEXPORT_NAME=createQtAppInstance -sFETCH=1 -sMAX_WEBGL_VERSION=2
   -sERROR_ON_UNDEFINED_SYMBOLS=1 -sASYNCIFY_IMPORTS=…` as `INTERFACE_LINK_OPTIONS`, placed after
   ours. The symptom: `node freecad_wasm_smoke.js` printed nothing and exited 0 (the script only
   defined a factory). `src/Wasm` empties that property.

e. **ICU data.** Base formats numbers with `icu::DecimalFormat`, so ICU and its data are
   mandatory. The data ships as a file (`/freecad/share/icu/icudt74l.dat`, found through
   `ICU_DATA`), trimmed from 30 MB to 2.6 MB. The first trim also dropped `curr/`, and App init
   failed with *"Failed to create ICU decimal-format symbols for: en_US"*:
   `DecimalFormatSymbols` needs the currency tree. Non-English locales fall back to root.

f. **FreeType and HarfBuzz are required**: `src/Mod/Part/App/Geometry.cpp` includes them
   unconditionally for its text-to-edges code (`FREECAD_USE_FREETYPE` only governs `FT2FC.cpp`).

g. **Boost.Thread**: `boost/thread/mutex.hpp` (Part's `Geometry.cpp`, Sketcher's `Constraint.cpp`)
   refuses to compile without `BOOST_HAS_PTHREADS`, which boost derives from `_POSIX_THREADS`, absent
   without `-pthread`. `FREECAD_WASM` defines it; Emscripten's single-threaded pthread functions
   serve a `boost::mutex` fine. (`BOOST_HAS_THREADS` too is wrong: libc++'s boost config sets it.)

h. **Link-time gaps**, all fixed: CPython's `make install` omits the bundled `libmpdec.a` and
   `libexpat.a` (≈450 undefined `mpd_*`/`PyExpat_XML_*`); Part's `SignalException.cpp` uses
   `boost::stacktrace` (`_Unwind_Backtrace`); the CrashReporter calls `fallocate` and `sigaltstack`.

i. **Paths.** No `/proc/self/exe`: `findHomePath()` is `$FREECAD_HOME` or `/freecad/`. The
   interpreter uses an *isolated* `PyConfig`, which ignores `PYTHONHOME`, so `config.home` is set
   from it. `share/Mod` (the resource directory) must be in the image: `Part::Feature` looks up
   the default material there (*"Material not found"* otherwise).

j. **Build-system traps**: Qt 6.4 cannot parse emsdk 6's `.emscripten` (pre-seeded); b2 records
   Boost as 64-bit unless told `address-model=32`; ICU 74's `config.sub` predates emscripten; the
   wasm Qt has no zstd, so rcc gets `--no-zstd` and FreeCAD's `--compress-algo zstd` collides
   (`FREECAD_RCC_COMPRESSION_ALGO=zlib`, level 9); GitHub archive tarballs (codeload) and therefore
   **all emscripten ports** are refused by the proxy — zlib is built from zlib.net, OCCT and
   yaml-cpp are `git clone`d. And, as in KiCad: **never two ninjas in one build dir** — I did it
   once by accident, and the second build's log was garbage.

## 4. Sizes

| file | raw | gzip -9 | brotli 9 |
| --- | ---: | ---: | ---: |
| `freecad_api.wasm` | 44.6 MB | 13.8 MB | 11.4 MB |
| `freecad_api.data` | 8.9 MB | 5.6 MB | 5.4 MB |
| `freecad_api.js` | 187 KB | 43 KB | |
| `freecad_wasm_smoke.wasm` | 44.0 MB | | |

`.data` = stdlib zip 4.0 MB (already deflated) + ICU 2.6 MB + `Mod/` 2.0 MB + `share/Mod` 1.4 MB
+ `Ext/` 52 KB. The FreeCAD static archives: Part 19 MB, App 16 MB, Base 12 MB, Sketcher 6 MB,
PartDesign 5 MB, Materials 4 MB, FreeCADApiCore 2.3 MB (object size, not wasm contribution).
Disk: prefix 554 MB, FreeCAD build tree 236 MB, emsdk 1.7 GB, tarballs 534 MB.

## 5. Times (4 cores, `-j3`, a native build sometimes competing)

| step | time |
| --- | ---: |
| emsdk 6.0.9 install | ~2 min |
| zlib, Eigen, fmt, yaml-cpp | < 1 min |
| ICU (host + wasm) | 2.6 min |
| Xerces-C | 5.8 min |
| Boost | 1.7–2.2 min |
| CPython (configure is ~300 emcc probes) | 3.3 min idle, ~12 min contended |
| qtbase | 4.7 min |
| OpenCASCADE | **30.8 min** (`-j3`); 42.6 min at `-j2` |
| FreeType + HarfBuzz | 1.1 min |
| FreeCAD, all 539 TUs + first link | 12.6 min |
| relink of one module (wasm-ld + wasm-opt + `.data`) | ~1.4 min |
| **total from scratch** | **≈ 1 h 20 min** |

## 6. Native build: unchanged

Every changed `.cpp` was preprocessed with the native build's own compile command
(`/home/user/build/freecad`) from the branch point and from `wasm-wip`: the token streams are
identical (`Application.cpp` differs in whitespace only). The first version of the
`Application.cpp` guards added `#ifdef` lines, which shifted the `__LINE__` values FreeCAD's log
macros embed — hence the single-line `FC_IF_THREADS()` / `FC_IF_NO_THREADS()` macros. The CMake
changes only act under `FREECAD_WASM` (the Qt component list and `src/Main` are unchanged with it
OFF). `src/Api` is untouched. Not done: an actual native rebuild — the other engineer's tree owns
`/home/user/build/freecad`.

## 7. Open issues and blockers, with proposed fixes

1. **No threads, so no parallelism.** OCCT was built without TBB, and its own thread pool
   (`OSD_Parallel`/`OSD_ThreadPool`) runs on the calling thread when no thread can be started;
   recomputes run inline. For a pthreads build:
   `-pthread` everywhere, a threaded Qt (6.4 supports it), `SharedArrayBuffer` (COOP/COEP headers
   in the page); `FC_NO_THREADS` then switches itself off.
2. **A trap kills the instance.** With `OCC_CONVERT_SIGNALS` gone, a null-pointer or integer
   divide in OCCT is a wasm trap (`RuntimeError`), not a `Standard_Failure`; the instance is dead
   afterwards (as in the KiCad transport: close and restart — `createFreecadApi` + `fcapi_init` is
   ~0.6 s). Floating-point errors do not trap in wasm.
3. **Upstream LLVM bug behind §3c.** Anything else that mixes `setjmp` and C++ `try` in one
   function under `-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1` will produce an invalid module.
   Check a fresh link with `node -e 'WebAssembly.validate(...)'` (or just run it). Worth a minimal
   repro for LLVM; trying the standardised EH encoding (`-sWASM_LEGACY_EXCEPTIONS=0`, needs node
   ≥ 22 / current browsers) may also avoid it.
4. **Locales**: only root/English ICU data; a German user gets `.` as decimal separator from ICU.
   Add locales to `trim-icu-data.sh`'s keep-list (≈10–40 KB each).
5. **Size** (11.4 MB brotli). Untried levers, cheapest first: serve compressed; `-Os`/`-Oz` for
   FreeCAD and OCCT (KiCad measured −17.5 % from `-Os`); drop TKIGES/TKSTEP if an app does not
   import (Part links them via `OCC_LIBRARIES`); exclude `Mod/*/Test*.py`, `parttests/`,
   `materialtests/` and `share/Mod/Material/Resources/Materials/*` cards an app does not need
   (≈2–3 MB of `.data`); trim the stdlib zip further (`email`, `asyncio`, `unittest`, …).
6. **Import module** (XCAF-based STEP/IGES with colours, `Import.*`) is not built: it needs OCCT's
   ApplicationFramework and DataExchange modules, i.e. XCAF → TKVCAF → TKV3d/TKService. OCCT's
   Visualization builds for Emscripten (it is how OCCT's own WebGL sample works); add those
   modules to the `occt` stage and `Import` to `FREECAD_WASM_MODULES` (+ a map entry in
   `src/Wasm/CMakeLists.txt`).
7. **Qt 6.4 is below Qt's supported emsdk** (3.1.14). Core/Xml are fine; if a newer qtbase is
   wanted, the host Qt (moc/rcc) must be built at the same version (`QT_HOST_PATH`).
8. **OCCT trace noise**: STEP/IGES transfers print their trace to stdout twice (OCCT's own
   messenger and FreeCAD's console). Cosmetic; set the OCCT message gravity or FreeCAD's log level.
9. `FREECAD_WASM` needs CMake ≥ 3.24 (`$<LINK_LIBRARY:WHOLE_ARCHIVE,…>` for `FreeCADApiCore`); the
   rest of FreeCAD needs 3.22.
10. `App::Application::getHomePath()` returns `/freecad//` — identical behaviour to native (the
    home path already ends in `/` and `getHomePath()` appends another); harmless.

## 8. Commits on `wasm-wip`

```
937e393 Wasm: smoke test covers STEP/IGES round trip and STL export; build.sh --api
762e225 Wasm: working freecad_wasm_smoke and freecad_api modules
cfdbed7 tools/wasm: build OCCT without OCC_CONVERT_SIGNALS; link CPython's own libs
2561455 App: keep Application.cpp line numbers unchanged for the no-threads guards
54dbfc7 CMake: FREECAD_WASM, one static WebAssembly module with built-in Python modules
272f96b Base, App, Part: build for Emscripten (FC_OS_WASM, FC_NO_THREADS)
4511cad tools/wasm: fix Boost, Qt, CPython and ICU stages; add FreeType and HarfBuzz
02d5660 Merge branch 'claude/freecad-repos-ipc-wasm-53j8he' into wasm-wip
d5b7bfa tools/wasm: emsdk pin, env and dependency build scripts for a WebAssembly build
```
