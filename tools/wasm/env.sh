#!/usr/bin/env bash
#
# Environment for the FreeCAD (application core, no GUI) WebAssembly build.
#
# Source this before running anything that compiles for wasm32-emscripten:
#
#     source tools/wasm/env.sh
#
# It activates the pinned emsdk (tools/wasm/install-emsdk.sh installs it),
# and exports the install prefix of the cross-compiled dependencies
# (tools/wasm/build-deps.sh fills it) plus the compile/link flags every
# library in that prefix was built with.
#
# Every dependency in $WASM_PREFIX was compiled with exactly $WASM_CFLAGS /
# $WASM_CXXFLAGS.  FreeCAD must use the same flags: mixing exception ABIs
# (-fexceptions, i.e. JavaScript exceptions, vs -fwasm-exceptions, i.e. the
# native wasm EH proposal) produces link errors that name personality /
# landing-pad / invoke_* symbols and never mention the flag.
#
# Layout (all overridable from the environment):
#
#   $EMSDK            /home/user/emsdk            pinned emsdk checkout
#   $WASM_ROOT        /home/user/wasm-build       everything this build writes
#   $WASM_PREFIX      $WASM_ROOT/prefix           static libs + headers of the deps
#   $WASM_HOST_PREFIX $WASM_ROOT/host             native host tools the cross builds need
#                                                 (ICU's data tools)
#   $WASM_SRC         $WASM_ROOT/src              downloaded tarballs (sources are
#                                                 unpacked under $WASM_ROOT/build and
#                                                 deleted after install)
#   $WASM_LOGS        $WASM_ROOT/logs             per-stage logs
#   $FREECAD_WASM_BUILD $WASM_ROOT/freecad        FreeCAD's own build tree

# ---------------------------------------------------------------------------
# pinned toolchain
# ---------------------------------------------------------------------------
# 6.0.9 is the version the KiCad fork's wasm module is built with, so both
# modules can share one emsdk.  install-emsdk.sh installs exactly this.
export WASM_EMSDK_VERSION="${WASM_EMSDK_VERSION:-6.0.9}"
export EMSDK="${EMSDK:-/home/user/emsdk}"

if ! command -v emcc >/dev/null 2>&1 || ! emcc --version 2>/dev/null | head -1 | grep -q " $WASM_EMSDK_VERSION "; then
    if [ -f "$EMSDK/emsdk_env.sh" ]; then
        # shellcheck source=/dev/null
        EMSDK_QUIET=1 source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    else
        echo "tools/wasm/env.sh: no emsdk at $EMSDK - run tools/wasm/install-emsdk.sh" >&2
    fi
fi

# ---------------------------------------------------------------------------
# locations
# ---------------------------------------------------------------------------
_WASM_ENV_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export WASM_REPO_ROOT="${WASM_REPO_ROOT:-$( cd "$_WASM_ENV_DIR/../.." && pwd )}"
export WASM_ROOT="${WASM_ROOT:-/home/user/wasm-build}"
export WASM_PREFIX="${WASM_PREFIX:-$WASM_ROOT/prefix}"
export WASM_HOST_PREFIX="${WASM_HOST_PREFIX:-$WASM_ROOT/host}"
export WASM_SRC="${WASM_SRC:-$WASM_ROOT/src}"
export WASM_BLD="${WASM_BLD:-$WASM_ROOT/build}"
export WASM_LOGS="${WASM_LOGS:-$WASM_ROOT/logs}"
export FREECAD_WASM_BUILD="${FREECAD_WASM_BUILD:-$WASM_ROOT/freecad}"

# Emscripten's own sysroot (the ports - zlib - install their libs here).
if command -v em-config >/dev/null 2>&1; then
    export EM_SYSROOT="${EM_SYSROOT:-$( em-config CACHE 2>/dev/null )/sysroot}"
fi

# ---------------------------------------------------------------------------
# host tools
# ---------------------------------------------------------------------------
# The build-time Python.  It runs FreeCAD's code generators and CPython's
# own build scripts; CPython's cross build insists it has the SAME minor
# version as the target (3.11).
export WASM_HOST_PYTHON="${WASM_HOST_PYTHON:-$( command -v python3.11 || true )}"
# Host Qt 6 whose moc/rcc/uic are used for the cross build.  Must be the
# same Qt version as the wasm qtbase (6.4.2 - Ubuntu 24.04's qt6-base-dev).
export WASM_QT_HOST_PATH="${WASM_QT_HOST_PATH:-/usr}"
export WASM_QT_HOST_CMAKE="${WASM_QT_HOST_CMAKE:-/usr/lib/x86_64-linux-gnu/cmake}"

# ---------------------------------------------------------------------------
# compile / link flags  (identical for every library in the prefix)
# ---------------------------------------------------------------------------
# -fwasm-exceptions: native wasm exception handling.  Required: FreeCAD and
#   OpenCASCADE throw everywhere, and OCCT's Standard_ErrorHandler also uses
#   setjmp/longjmp, which -fwasm-exceptions turns into wasm EH as well
#   (SUPPORT_LONGJMP=wasm).  C libraries get it too so that any setjmp in
#   them uses the same mechanism.
# -sWASM_LEGACY_EXCEPTIONS=1: stated explicitly, as in the KiCad build: it is
#   6.0.9's default anyway, and Node 20 cannot instantiate modules built with
#   the standardised (exnref) encoding.
# No -pthread anywhere: this is a single-threaded build (-sUSE_PTHREADS=0 is
#   the default).
# zlib is NOT the emscripten port: ports download from codeload.github.com,
#   which the build proxy refuses, so build-deps.sh builds zlib into the prefix.
export WASM_OPT_LEVEL="${WASM_OPT_LEVEL:--O2}"
export WASM_COMMON_FLAGS="-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1"
export WASM_CFLAGS="$WASM_OPT_LEVEL $WASM_COMMON_FLAGS"
export WASM_CXXFLAGS="$WASM_OPT_LEVEL $WASM_COMMON_FLAGS"
export WASM_LDFLAGS="$WASM_OPT_LEVEL $WASM_COMMON_FLAGS"

# What a final module (freecad_api, freecad_wasm_smoke) wants on top of
# WASM_LDFLAGS.  The stack default (64 KB) is far too small for OCCT's
# boolean operations and CPython's recursion; memory grows on demand.
export WASM_MODULE_LDFLAGS="-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=8MB -sFORCE_FILESYSTEM=1"

# ---------------------------------------------------------------------------
# dependency discovery
# ---------------------------------------------------------------------------
# The emscripten toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE/LIBRARY/
# INCLUDE to ONLY, so CMAKE_PREFIX_PATH alone is not enough: the prefix and
# the emscripten sysroot must be find ROOTS too, or find_package() silently
# misses them.
export WASM_FIND_ROOT_PATH="$WASM_PREFIX;${EM_SYSROOT:-}"
export PKG_CONFIG_PATH="$WASM_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$WASM_PREFIX/lib/pkgconfig"

# CMake arguments common to every CMake-based dependency.
export WASM_CMAKE_ARGS="\
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_INSTALL_PREFIX=$WASM_PREFIX \
-DCMAKE_PREFIX_PATH=$WASM_PREFIX \
-DCMAKE_FIND_ROOT_PATH=$WASM_FIND_ROOT_PATH \
-DBUILD_SHARED_LIBS=OFF \
-DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
-DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_CXX_SCAN_FOR_MODULES"

unset _WASM_ENV_DIR
