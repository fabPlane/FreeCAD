# FreeCAD's application core in WebAssembly

`tools/wasm/` builds FreeCAD's application core — Base, App, Materials, Part, Sketcher,
PartDesign and the API server's C ABI (`src/Api`) — for `wasm32-emscripten`, as one statically
linked module that runs in Node or in a browser tab. There is no GUI. This is the FreeCAD
counterpart of the KiCad fork's `tools/wasm/` and follows its structure.

Current state, sizes and timings: [STATUS.md](STATUS.md).

## Quick start

```sh
tools/wasm/install-emsdk.sh      # emsdk 6.0.9 -> /home/user/emsdk            (~2 min)
tools/wasm/build-deps.sh         # all dependencies -> /home/user/wasm-build/prefix
tools/wasm/configure.sh          # emcmake cmake -> /home/user/wasm-build/freecad
tools/wasm/build.sh --smoke      # freecad_wasm_smoke, then run it under node
tools/wasm/build.sh --api        # freecad_api.{js,wasm,data}, then test-api.mjs under node
```

From nothing to a running module: about 1 h 20 min on 4 cores at `-j3` (OCCT alone is 31 min; see STATUS.md).

Every script sources `env.sh`; override its locations from the environment (`EMSDK`,
`WASM_ROOT`, `WASM_PREFIX`, `FREECAD_WASM_BUILD`, `JOBS`).

## Files

| file | what |
| --- | --- |
| `env.sh` | activates the pinned emsdk; prefix layout; the flags every library is compiled with |
| `install-emsdk.sh` | clones emsdk and installs/activates `WASM_EMSDK_VERSION` (6.0.9) |
| `build-deps.sh` | the dependencies, one idempotent stage each (`list` shows what is done) |
| `make-python-stdlib-zip.py` | CPython's stdlib as a `.pyc`-only zip for MEMFS |
| `trim-icu-data.sh` | ICU's data archive cut down to root + English (30 MB → 2.6 MB) |
| `patches/` | `occt-7.6.3-emscripten-no-signal-conversion.patch` |
| `test-api.mjs` | drives `freecad_api` through `fcapi_*` from node (14 checks) |
| `configure.sh` | `emcmake cmake` of FreeCAD with `FREECAD_WASM=ON` |
| `build.sh` | builds the wasm targets (`--smoke` / `--api` also run the tests) |
| `STATUS.md` | what works, what does not, sizes, times, blockers |

## Toolchain and flags

- **Emscripten 6.0.9**, the version the KiCad wasm module is built with, so one emsdk serves both.
- `-O2 -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1` on **every** library, C ones included.
  FreeCAD and OpenCASCADE throw; OCCT and CPython also `setjmp`, which `-fwasm-exceptions`
  turns into wasm EH too. Mixing exception ABIs links but fails in confusing ways. The legacy
  encoding is what Node 20 can instantiate.
- **No threads** (`-sUSE_PTHREADS=0`, the default). `FCConfig.h` defines `FC_NO_THREADS` for
  Emscripten without `-pthread`; see "Source changes" below.
- Static libraries only.

## Dependencies (`build-deps.sh`)

| stage | version | notes |
| --- | --- | --- |
| `zlib` | 1.3.1 | built here, not the emscripten port: ports download from codeload.github.com, which the proxy refuses |
| `eigen` | 3.4.0 | headers; installed with the **host** cmake (its blas/ probes Fortran under emcmake) |
| `fmt` | 11.0.2 | |
| `yamlcpp` | 0.8.0 | Materials |
| `icu` | 74.2 | Base needs `uc` + `i18n`. Host build first (`--with-cross-build`, the system `config.sub`, a `linux` host triple), data as a file (`--with-data-packaging=archive`) |
| `icudata` | 74.2 | `trim-icu-data.sh`: root + English in every tree, no collation, break iterators, transliteration, rbnf or table converters: 30 MB → 2.6 MB. **`curr/` must stay** — `DecimalFormatSymbols` fails without it |
| `xerces` | 3.2.5 | transcoder **ICU** (musl's "C" locale makes the iconv one reject non-ASCII), no network, no threads |
| `boost` | 1.86.0 | b2 `toolset=emscripten address-model=32` (else its CMake configs claim 64-bit and are rejected): program_options, regex, thread, date_time, filesystem, system, atomic, chrono + headers |
| `python` | 3.11.15 | CPython's own recipe (`Tools/wasm/config.site-wasm32-emscripten`, `--with-build-python=python3.11`), all extension modules static in `libpython3.11.a`; `_bz2 _lzma _sqlite3 _ctypes _ssl _hashlib readline curses dbm tkinter` and the test modules left out. The bundled libmpdec and expat are installed as `libpython3.11-{mpdec,expat}.a` (`make install` skips them). Stdlib: `lib/python311.zip`, 483 modules as `.pyc`, 4.0 MB |
| `qt` | 6.4.2 | qtbase, `-platform wasm-emscripten -no-gui`, Core + Xml. Must equal the host Qt (Ubuntu 24.04's 6.4.2) whose moc/rcc the cross build runs. No threads ⇒ no QFuture ⇒ no Qt Concurrent. Qt 6.4 cannot parse emsdk 6's `.emscripten`; its auto-detection is pre-seeded |
| `occt` | 7.6.3 | same as the native build. FoundationClasses, ModelingData, ModelingAlgorithms + `TKXSBase TKSTEPBase TKSTEPAttr TKSTEP209 TKSTEP TKIGES TKSTL` via `BUILD_ADDITIONAL_TOOLKITS` (the DataExchange *module* would drag in XCAF → Visualization → FreeType). No Draw, Tcl/Tk, OpenGL. **Patched: no `OCC_CONVERT_SIGNALS`** (see STATUS.md §3) |
| `freetype` | 2.13.3 | Part's `Geometry.cpp` includes FreeType and HarfBuzz unconditionally (text to edges) |
| `harfbuzz` | 8.3.0 | with FreeType, no GLib/ICU |

Sources are downloaded to `$WASM_ROOT/src`, unpacked and built under `$WASM_ROOT/build` and
**deleted after a successful install** (disk is tight; `KEEP_BUILD=1` keeps them). Logs:
`$WASM_ROOT/logs/<stage>-{configure,build,install}.log`. Stamps: `$WASM_ROOT/.stamp-<stage>`.

## The `FREECAD_WASM` CMake option

`cMake/FreeCAD_Helpers/SetupWasm.cmake`, default **OFF**; with it OFF nothing changes.

Natively, `Part`, `Sketcher`, ... are shared libraries that Python `dlopen`s on `import Part`.
With `FREECAD_WASM=ON` (Emscripten toolchain required):

1. **Everything is static.** The global `TARGET_SUPPORTS_SHARED_LIBS` property is set to FALSE,
   so every `add_library(X SHARED|MODULE)` in the tree builds a static archive (CMake prints one
   warning per target). No module's CMakeLists had to change.
2. **Only `FREECAD_WASM_MODULES` are built** (default `Material;Part;Sketcher;PartDesign`); every
   other `BUILD_<MODULE>` is forced OFF, as are `BUILD_GUI` and the developer tests. `src/Main`
   (desktop executables) is skipped.
3. **Modules become Python built-ins.** `src/Wasm/CMakeLists.txt` generates
   `FreeCADWasmModules.cpp` from the module list:

   ```c++
   extern "C" PyObject* PyInit_Part(void);   // ... one per module
   void FreeCADWasm_registerBuiltinModules(void) {
       PyImport_AppendInittab("Part", PyInit_Part);   // ...
   }
   ```

   It must run before `Py_Initialize()`, i.e. before `App::Application::init()`. `import Part`
   then resolves through `BuiltinImporter`, which comes before any path-based finder. Referencing
   `PyInit_<name>` is also what pulls each module's objects into the link.
   To add a module: add it to `FREECAD_WASM_MODULES` and its `target;pyname` pair to the map at
   the top of `src/Wasm/CMakeLists.txt`.
4. **The file system is preloaded** into `<target>.data` and mounted at `/freecad`:

   | MEMFS | from |
   | --- | --- |
   | `/freecad/Mod/…` | `<build>/Mod` (each module's Python files, material cards, …) |
   | `/freecad/Ext/…` | `<build>/Ext` (the `freecad` namespace package) |
   | `/freecad/share/Mod/…` | `<build>/share/Mod` — the resource directory (`RESOURCEDIR=share`): material cards and models; `Part::Feature` needs the default material |
   | `/freecad/lib/python311.zip` | `FREECAD_WASM_PYTHON_STDLIB_ZIP` |
   | `/freecad/share/icu/icudt74l.dat` | `FREECAD_WASM_ICU_DATA` |

   `FreeCADWasm_setupEnvironment()` sets `FREECAD_HOME=/freecad`, `PYTHONHOME=/freecad`,
   `ICU_DATA=/freecad/share/icu`, `FREECAD_USER_HOME=/home/web_user` (each only if unset) and the
   `C.UTF-8` locale.
5. **Boost.Thread sees pthreads**: `BOOST_HAS_PTHREADS` is defined (boost/config derives it from
   `_POSIX_THREADS`, which is absent without `-pthread`); Emscripten's single-threaded pthread
   functions are enough for the `boost::mutex` in Part and Sketcher.
6. **Qt's application link flags are dropped.** A wasm Qt adds `-sMODULARIZE=1
   -sEXPORT_NAME=createQtAppInstance`, `FETCH`, WebGL and ASYNCIFY options to every executable
   linking `Qt6::Core`, *after* ours; `src/Wasm` empties `Qt6::Platform`'s
   `INTERFACE_LINK_OPTIONS` so the targets own their flags.

### Targets (`src/Wasm/`)

- `FreeCADWasm` — static library: `FreeCADWasm_setupEnvironment()`,
  `FreeCADWasm_registerBuiltinModules()`, `FreeCADWasm_builtinModuleNames()` (`FreeCADWasm.h`).
- `freecad_wasm_smoke` — `node freecad_wasm_smoke.js`: initialises `App::Application`, creates a
  document, adds a `Part::Box` from C++, recomputes, checks the volume; then from Python a boolean
  cut, a PartDesign Body/Sketch/Pad, a STEP/IGES round trip and STL export in MEMFS, and checks
  that the modules are built-ins.
- `freecad_api` (when `BUILD_API=ON`) — `freecad_api.js` + `.wasm` + `.data`, the `fcapi_*` C ABI
  of `src/Api/ApiC.cpp`. `main()` (`freecad_api_main.cpp`) runs at instantiation and does the two
  calls above, so `fcapi_init()` finds everything registered. Link flags: `MODULARIZE=1`,
  `EXPORT_ES6=1`, `EXPORT_NAME=createFreecadApi`,
  `EXPORTED_FUNCTIONS=_main,_malloc,_free,_fcapi_init,_fcapi_dispatch,_fcapi_free,_fcapi_shutdown,_fcapi_last_error`,
  `EXPORTED_RUNTIME_METHODS=HEAPU8,FS,UTF8ToString,stringToNewUTF8,ccall,cwrap`,
  `ALLOW_MEMORY_GROWTH=1`, `INITIAL_MEMORY=128MB`, `STACK_SIZE=8MB`, `ENVIRONMENT=node,web,worker`.
  `FreeCADApiCore` is linked whole-archive.

### Source changes (all guarded; the native build is unchanged with the option OFF)

| file | change |
| --- | --- |
| `src/FCConfig.h` | `__EMSCRIPTEN__` ⇒ `FC_OS_LINUX` + `FC_OS_WASM` (+ `FC_NO_THREADS` without `-pthread`); it used to hit `#error "not ported"` |
| `src/App/Application.cpp` | `FC_NO_THREADS`: no recompute worker thread (`std::thread` cannot start one), requests are processed inline, `isAsyncRecomputeEnabled()` is false. Written with the single-line `FC_IF_THREADS()` / `FC_IF_NO_THREADS()` macros so no `__LINE__` shifts |
| `src/App/ApplicationDirectories.cpp` | `FC_OS_WASM`: `findHomePath()` is `$FREECAD_HOME` or `/freecad/` (no `/proc/self/exe`) |
| `src/Base/Interpreter.cpp` | `FC_OS_WASM`: the isolated `PyConfig` ignores `PYTHONHOME`, so set `config.home` from it (default `/freecad`) |
| `src/Base/SystemHandler.cpp` | `FC_OS_WASM`: no `<execinfo.h>` |
| `src/Base/CrashReporter/Writer.cpp` | `FC_OS_WASM`: no `fallocate`, no signal handlers (no `sigaltstack`) |
| `src/Mod/Part/App/SignalException.cpp` | `FC_OS_WASM`: no signal→OCCT-exception handlers (`boost::stacktrace` needs `_Unwind_Backtrace`) |
| `cMake/FreeCAD_Helpers/SetupQt.cmake` | `FREECAD_WASM`: Qt components Core, Xml, LinguistTools (no Concurrent, Network) |
| `cMake/FreeCAD_Helpers/SetupWasm.cmake` | the option itself (new file) |
| `src/Wasm/` | glue, generated module registration, the two link targets (new) |

Checked by preprocessing every changed `.cpp` with the native build's own compile command before
and after: the token streams are identical (Application.cpp differs only in whitespace).
`src/Api` is **not** modified.
| `CMakeLists.txt`, `src/CMakeLists.txt` | call `SetupWasm()`; skip `src/Main`, add `src/Wasm` under `FREECAD_WASM` |

## Running

```sh
cd /home/user/wasm-build/freecad/wasm
node freecad_wasm_smoke.js                        # 9 checks, ~0.8 s
node /path/to/tools/wasm/test-api.mjs .           # 14 checks
```

```js
import createFreecadApi from "./freecad_api.js";
const m = await createFreecadApi();          // main() has registered the modules
const cfg = m.stringToNewUTF8("{}");
if (m._fcapi_init(cfg) !== 0) throw new Error(m.UTF8ToString(m._fcapi_last_error()));
```

Events: `src/Api/ApiC.cpp` calls `Module.__fcapiEvent(bytes)` (install it before `fcapi_init`).
