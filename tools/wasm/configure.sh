#!/usr/bin/env bash
#
# Configure FreeCAD's application core for wasm32-emscripten.
#
#   tools/wasm/configure.sh [extra cmake args...]
#
# Needs the prefix from tools/wasm/build-deps.sh.  Build tree:
# $FREECAD_WASM_BUILD (default /home/user/wasm-build/freecad).  Then
#
#   tools/wasm/build.sh                    # libraries + freecad_wasm_smoke
#
# Everything non-obvious is explained in env.sh, build-deps.sh and
# cMake/FreeCAD_Helpers/SetupWasm.cmake.
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=env.sh
source "$HERE/env.sh"

P="$WASM_PREFIX"
BUILD="$FREECAD_WASM_BUILD"
PY_MM=3.11
PY_ZIP="$P/lib/python${PY_MM//./}.zip"
ICU_DAT="$( ls "$P"/share/icu/icudt*l.dat 2>/dev/null | head -1 )"

for f in "$P/lib/libpython$PY_MM.a" "$PY_ZIP" "$P/lib/libQt6Core.a" "$P/lib/libTKernel.a" \
         "$P/lib/libxerces-c.a" "$P/lib/libboost_program_options.a" "$P/lib/libicuuc.a" "$ICU_DAT"; do
    [ -e "$f" ] || { echo "missing $f - run tools/wasm/build-deps.sh" >&2; exit 1; }
done

# Optimisation for FreeCAD's own translation units (the deps are -O2).
# CMake appends CMAKE_CXX_FLAGS_RELEASE after CMAKE_CXX_FLAGS, so it has to
# be set there or the release default (-O3) silently wins.
FC_OPT="${WASM_FC_OPT_LEVEL:--O2}"

# CPython's static archive does not record its own dependencies.  zlib: the
# zlib module; libicudata: ICU's (empty) data stub - the data is a file.
EXTRA_LIBS="$P/lib/libz.a;$P/lib/libicudata.a"

# shellcheck disable=SC2086
emcmake cmake -S "$WASM_REPO_ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$WASM_CFLAGS" \
    -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
    -DCMAKE_C_FLAGS_RELEASE="$FC_OPT -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FC_OPT -DNDEBUG" \
    -DCMAKE_EXE_LINKER_FLAGS="$WASM_LDFLAGS" \
    -DCMAKE_PREFIX_PATH="$P" \
    -DCMAKE_FIND_ROOT_PATH="$WASM_FIND_ROOT_PATH" \
    -DCMAKE_INSTALL_PREFIX="$BUILD/install" \
    -DFREECAD_USE_CCACHE=OFF \
    \
    -DFREECAD_WASM=ON \
    -DFREECAD_WASM_MODULES="Material;Part;Sketcher;PartDesign" \
    -DFREECAD_WASM_PYTHON_STDLIB_ZIP="$PY_ZIP" \
    -DFREECAD_WASM_ICU_DATA="$ICU_DAT" \
    -DFREECAD_WASM_EXTRA_LIBS="$EXTRA_LIBS" \
    -DBUILD_GUI=OFF \
    -DFREECAD_USE_FREETYPE=OFF \
    -DFREECAD_USE_EXTERNAL_PYCXX=OFF \
    -DFREECAD_CHECK_PIVY=OFF \
    -DBUILD_DYNAMIC_LINK_PYTHON=ON \
    -DINSTALL_TO_SITEPACKAGES=OFF \
    \
    -DPython3_EXECUTABLE="$WASM_HOST_PYTHON" \
    -DPython3_INCLUDE_DIR="$P/include/python$PY_MM" \
    -DPython3_LIBRARY="$P/lib/libpython$PY_MM.a" \
    \
    -DQt6_DIR="$P/lib/cmake/Qt6" \
    -DQT_HOST_PATH="$WASM_QT_HOST_PATH" \
    -DQT_HOST_PATH_CMAKE_DIR="$WASM_QT_HOST_CMAKE" \
    \
    -DOpenCASCADE_DIR="$P/lib/cmake/opencascade" \
    -DXercesC_DIR="$P/lib/cmake/XercesC" \
    -DBoost_DIR="$P/lib/cmake/Boost-1.86.0" \
    -DICU_ROOT="$P" \
    -Dyaml-cpp_DIR="$P/lib/cmake/yaml-cpp" \
    -DEigen3_DIR="$P/share/eigen3/cmake" \
    -DZLIB_ROOT="$P" \
    "$@"
