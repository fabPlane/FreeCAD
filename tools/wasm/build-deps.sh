#!/usr/bin/env bash
#
# Build FreeCAD's application-core dependencies for WebAssembly
# (wasm32-emscripten, static libraries, single-threaded).
#
#   tools/wasm/build-deps.sh              # every stage, in order
#   tools/wasm/build-deps.sh occt         # one stage
#   tools/wasm/build-deps.sh icu xerces   # several stages
#   tools/wasm/build-deps.sh list         # stages and whether each is done
#
# Stages (in dependency order):
#   zlib eigen fmt yamlcpp icu xerces boost python qt occt
#
# Everything lands under $WASM_ROOT (default /home/user/wasm-build, see env.sh):
#   src/      downloaded tarballs (kept; they are the only thing re-used)
#   build/    unpacked sources + build trees; DELETED after each successful
#             install (disk is tight) unless KEEP_BUILD=1
#   prefix/   the install prefix FreeCAD's configure points at
#   host/     native host tools a cross build needs (ICU's data tools)
#   logs/     per-stage configure/build/install logs
#
# A stage that completed leaves $WASM_ROOT/.stamp-<stage> and is skipped next
# time; delete the stamp to rebuild it.
#
# ---------------------------------------------------------------------------
# Things that were NOT obvious, and that this script therefore encodes:
#
#  1. -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1 on EVERY library, C ones
#     too (env.sh).  OCCT and FreeCAD throw; OCCT and CPython also setjmp,
#     and with -fwasm-exceptions setjmp/longjmp use wasm EH as well.  A
#     library compiled without it links but aborts at the first longjmp.
#
#  2. codeload.github.com (GitHub "archive" tarballs, and therefore every
#     emscripten port: -sUSE_ZLIB, -sUSE_ICU, -sUSE_BOOST_HEADERS ...) is
#     refused by the build proxy.  zlib and ICU are built here instead of
#     using the ports; GitHub-hosted sources without release assets (OCCT,
#     yaml-cpp) are fetched with a shallow `git clone` of the tag.
#
#  3. ICU is required by FreeCAD's Base (NumericFormatting uses
#     icu::DecimalFormat).  Its data is NOT compiled into the library
#     (--with-data-packaging=archive): we ship icudt74l.dat trimmed to the
#     root/en locales with the host's icupkg, and FreeCAD finds it through
#     ICU_DATA at run time.  The cross build needs a native ICU build of the
#     same version (--with-cross-build); it is built first in build/icu-host.
#
#  4. Xerces-C uses ICU as its transcoder, not iconv: musl starts in the "C"
#     locale where mbstowcs rejects every non-ASCII byte.
#
#  5. CPython 3.11 is cross-compiled with its own recipe
#     (Tools/wasm/config.site-wasm32-emscripten + --with-build-python, the
#     same thing Tools/wasm/wasm_build.py runs), but zlib/bzip2/sqlite are
#     NOT the ports (note 2): zlib comes from the prefix, _bz2/_sqlite3/
#     _ctypes/_decimal-with-libmpdec-system are disabled.  All extension
#     modules are built into libpython3.11.a (MODULE_BUILDTYPE=static is
#     what configure picks for Emscripten).
#
#  6. Qt must be the same version as the host Qt whose moc/rcc the cross
#     build runs (QT_HOST_PATH).  The host is Ubuntu 24.04's Qt 6.4.2, so the
#     target is qtbase 6.4.2, configured -platform wasm-emscripten with
#     -no-gui and no threads (the wasm default).  Without QT_FEATURE_thread
#     there is no QFuture and so no Qt Concurrent: FreeCAD's FREECAD_WASM
#     option copes with that (src/Mod/Material).
#
#  7. OpenCASCADE: 7.6.3, the same version the native build uses.  Only the
#     FoundationClasses/ModelingData/ModelingAlgorithms modules are enabled,
#     and the data-exchange toolkits FreeCAD's Part needs are added one by
#     one with BUILD_ADDITIONAL_TOOLKITS - enabling the DataExchange MODULE
#     would drag in TKXCAF -> TKVCAF -> TKV3d/TKService (Visualization) and
#     FreeType.
# ---------------------------------------------------------------------------

set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=env.sh
source "$HERE/env.sh"

# ---------------------------------------------------------------------------
# versions
# ---------------------------------------------------------------------------
ZLIB_VERSION=1.3.1
EIGEN_VERSION=3.4.0
FMT_VERSION=11.0.2
YAMLCPP_VERSION=0.8.0
ICU_VERSION=74.2            # = Ubuntu 24.04's libicu, so the host icupkg matches
XERCES_VERSION=3.2.5
BOOST_VERSION=1.86.0
PYTHON_VERSION=3.11.15      # = the native build's python3.11 (build-python must match)
QT_VERSION=6.4.2            # = the host Qt 6 (moc/rcc)
OCCT_VERSION=7.6.3          # = the native build's OCCT

ICU_U="${ICU_VERSION//./_}"                 # 74_2
ICU_MAJOR="${ICU_VERSION%%.*}"              # 74
BOOST_U="${BOOST_VERSION//./_}"             # 1_86_0
PY_MM="${PYTHON_VERSION%.*}"                # 3.11
QT_MM="${QT_VERSION%.*}"                    # 6.4

JOBS="${JOBS:-3}"
KEEP_BUILD="${KEEP_BUILD:-0}"

mkdir -p "$WASM_SRC" "$WASM_BLD" "$WASM_LOGS" "$WASM_PREFIX" "$WASM_HOST_PREFIX"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die()  { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

stamp_file() { echo "$WASM_ROOT/.stamp-$1"; }
is_done()    { [ -f "$( stamp_file "$1" )" ]; }
mark_done()  { date > "$( stamp_file "$1" )"; }

# run <log-name> <command...>: output to $WASM_LOGS/<log-name>.log; on
# failure show the tail and stop.
run() {
    local log="$WASM_LOGS/$1.log"; shift
    if ! "$@" > "$log" 2>&1; then
        tail -40 "$log" >&2
        die "failed: $* (full log: $log)"
    fi
}

fetch() {
    local url=$1 file=${2:-$( basename "$1" )}
    if [ ! -f "$WASM_SRC/$file" ]; then
        echo "fetching $file"
        curl -fsSL -o "$WASM_SRC/$file.part" "$url"
        mv "$WASM_SRC/$file.part" "$WASM_SRC/$file"
    fi
}

# fetch_git <url> <tag> <dir>: shallow clone of one tag, into src/<dir>
# (for GitHub projects without release assets - see note 2).
fetch_git() {
    local url=$1 tag=$2 dir=$3
    if [ ! -d "$WASM_SRC/$dir" ]; then
        echo "cloning $url@$tag"
        git clone -q --depth 1 -b "$tag" "$url" "$WASM_SRC/$dir.part"
        rm -rf "$WASM_SRC/$dir.part/.git"
        mv "$WASM_SRC/$dir.part" "$WASM_SRC/$dir"
    fi
}

# unpack <tarball> <dir-name-inside>: fresh copy under build/, echoes the path
unpack() {
    local file=$1 dir=$2
    rm -rf "${WASM_BLD:?}/$dir"
    case "$file" in
        *.zip) unzip -q "$WASM_SRC/$file" -d "$WASM_BLD" ;;
        *)     tar xf "$WASM_SRC/$file" -C "$WASM_BLD" ;;
    esac
    echo "$WASM_BLD/$dir"
}

cleanup() {
    if [ "$KEEP_BUILD" != 1 ]; then
        for d in "$@"; do rm -rf "${WASM_BLD:?}/$d"; done
    fi
}

cmake_dep() {   # cmake_dep <name> <src> [cmake args...]
    local name=$1 src=$2; shift 2
    # shellcheck disable=SC2086
    run "$name-configure" emcmake cmake -S "$src" -B "$WASM_BLD/$name-build" -G Ninja \
        $WASM_CMAKE_ARGS \
        -DCMAKE_C_FLAGS="$WASM_CFLAGS" \
        -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$WASM_LDFLAGS" \
        "$@"
    run "$name-build"   cmake --build   "$WASM_BLD/$name-build" -j"$JOBS"
    run "$name-install" cmake --install "$WASM_BLD/$name-build"
}

preflight() {
    command -v emcc    >/dev/null || die "emcc not found - run tools/wasm/install-emsdk.sh"
    command -v cmake   >/dev/null || die "cmake not found"
    command -v ninja   >/dev/null || die "ninja not found"
    [ -x "$WASM_HOST_PYTHON" ]    || die "no host python$PY_MM (WASM_HOST_PYTHON)"
    echo "emcc:   $( emcc --version | head -1 )"
    echo "cmake:  $( cmake --version | head -1 )"
    echo "python: $( "$WASM_HOST_PYTHON" --version ) ($WASM_HOST_PYTHON, build python)"
    echo "prefix: $WASM_PREFIX"
    echo "flags:  $WASM_CXXFLAGS"
    echo "disk:   $( df -h "$WASM_ROOT" | awk 'NR==2 {print $4 " free"}' )"
}

# ---------------------------------------------------------------------------
# zlib  (instead of the emscripten port - note 2)
# ---------------------------------------------------------------------------
stage_zlib() {
    fetch "https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz"
    local src; src=$( unpack "zlib-$ZLIB_VERSION.tar.gz" "zlib-$ZLIB_VERSION" )
    cmake_dep zlib "$src" -DZLIB_BUILD_EXAMPLES=OFF
    # zlib's CMake always builds and installs the shared flavour as well;
    # under emscripten that is just another static archive - drop it so
    # nothing picks it up by accident.
    rm -f "$WASM_PREFIX"/lib/libz.so*
    ls "$WASM_PREFIX/lib/libz.a" >/dev/null
    cleanup "zlib-$ZLIB_VERSION" zlib-build
}

# ---------------------------------------------------------------------------
# Eigen (headers only - installed through its CMake so Eigen3Config exists).
# Configured with the HOST cmake/compiler: it is architecture independent,
# and under emcmake its blas/ subdirectory probes for a Fortran compiler and
# hands the host gfortran our emcc flags.
# ---------------------------------------------------------------------------
stage_eigen() {
    fetch "https://gitlab.com/libeigen/eigen/-/archive/$EIGEN_VERSION/eigen-$EIGEN_VERSION.tar.bz2"
    local src; src=$( unpack "eigen-$EIGEN_VERSION.tar.bz2" "eigen-$EIGEN_VERSION" )
    run eigen-configure env -u CFLAGS -u CXXFLAGS -u LDFLAGS cmake -S "$src" -B "$WASM_BLD/eigen-build" \
        -DCMAKE_INSTALL_PREFIX="$WASM_PREFIX" -DBUILD_TESTING=OFF -DEIGEN_BUILD_DOC=OFF \
        -DEIGEN_BUILD_PKGCONFIG=ON -DCMAKE_Fortran_COMPILER=NOTFOUND
    run eigen-install cmake --install "$WASM_BLD/eigen-build"
    ls "$WASM_PREFIX/share/eigen3/cmake/Eigen3Config.cmake" >/dev/null
    cleanup "eigen-$EIGEN_VERSION" eigen-build
}

# ---------------------------------------------------------------------------
# fmt
# ---------------------------------------------------------------------------
stage_fmt() {
    fetch "https://github.com/fmtlib/fmt/releases/download/$FMT_VERSION/fmt-$FMT_VERSION.zip"
    local src; src=$( unpack "fmt-$FMT_VERSION.zip" "fmt-$FMT_VERSION" )
    cmake_dep fmt "$src" -DFMT_DOC=OFF -DFMT_TEST=OFF -DFMT_INSTALL=ON
    ls "$WASM_PREFIX/lib/libfmt.a" >/dev/null
    cleanup "fmt-$FMT_VERSION" fmt-build
}

# ---------------------------------------------------------------------------
# yaml-cpp
# ---------------------------------------------------------------------------
stage_yamlcpp() {
    fetch_git https://github.com/jbeder/yaml-cpp "$YAMLCPP_VERSION" "yaml-cpp-$YAMLCPP_VERSION"
    cmake_dep yamlcpp "$WASM_SRC/yaml-cpp-$YAMLCPP_VERSION" \
        -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_CPP_BUILD_CONTRIB=OFF \
        -DYAML_BUILD_SHARED_LIBS=OFF
    ls "$WASM_PREFIX/lib/libyaml-cpp.a" >/dev/null
    cleanup yamlcpp-build
}

# ---------------------------------------------------------------------------
# ICU (note 3)
# ---------------------------------------------------------------------------
stage_icu() {
    fetch "https://github.com/unicode-org/icu/releases/download/release-${ICU_VERSION//./-}/icu4c-$ICU_U-src.tgz"
    local src="$WASM_BLD/icu"
    [ -d "$src" ] || unpack "icu4c-$ICU_U-src.tgz" icu >/dev/null
    src="$src/source"

    # 1. native build: the cross build runs its tools (and reads
    #    config/icucross.mk from it).  Kept until the wasm build succeeded.
    if [ ! -x "$WASM_BLD/icu-host/bin/icupkg" ]; then
        mkdir -p "$WASM_BLD/icu-host"
        ( cd "$WASM_BLD/icu-host" &&
          run icu-host-configure env -u CFLAGS -u CXXFLAGS -u LDFLAGS \
              "$src/configure" --disable-tests --disable-samples --disable-extras \
              --disable-icuio --disable-layoutex --enable-static --disable-shared \
              --prefix="$WASM_HOST_PREFIX" &&
          run icu-host-build make -j"$JOBS" )
    fi

    # ICU 74's config.sub predates emscripten; the system's knows it.  The
    # host triple still has to say "linux": ICU's configure maps anything it
    # does not know to the unsupported mh-unknown fragment, and mh-linux is
    # the right one for emcc (a clang with a GNU-ish driver).  The C/C++
    # platform detection is done from __EMSCRIPTEN__ in unicode/platform.h,
    # not from the triple.
    cp /usr/share/misc/config.sub "$src/config.sub"

    # 2. wasm build.  archive packaging: libicudata is a stub and the data
    #    comes from icudt<N>l.dat at run time (ICU_DATA).
    mkdir -p "$WASM_BLD/icu-wasm"
    ( cd "$WASM_BLD/icu-wasm" &&
      run icu-configure env CFLAGS="$WASM_CFLAGS" CXXFLAGS="$WASM_CXXFLAGS -std=c++17" \
          LDFLAGS="$WASM_LDFLAGS" \
          emconfigure "$src/configure" --host=wasm32-unknown-linux-gnu \
          --with-cross-build="$WASM_BLD/icu-host" \
          --enable-static --disable-shared --disable-tools --disable-tests \
          --disable-samples --disable-extras --disable-icuio --disable-layoutex \
          --disable-dyload --with-data-packaging=archive \
          --prefix="$WASM_PREFIX" &&
      run icu-build emmake make -j"$JOBS" &&
      run icu-install emmake make install )

    # 3. trimmed data: only the root/en locales and the locale-independent
    #    tables survive.  FreeCAD looks up number symbols for the C/en_US_POSIX
    #    locale only; everything else falls back to root.
    local dat="$src/data/in/icudt${ICU_MAJOR}l.dat"
    local icupkg="$WASM_BLD/icu-host/bin/icupkg"
    local out="$WASM_PREFIX/share/icu/icudt${ICU_MAJOR}l.dat"
    mkdir -p "$WASM_PREFIX/share/icu"
    "$icupkg" -l "$dat" > "$WASM_BLD/icu-items.txt"
    # keep: everything that is not a per-locale resource bundle, plus root/en*
    # bundles.  Removed: coll/ (collation), brkitr/ dictionaries, zone/, curr/,
    # lang/, region/, unit/, rbnf/, translit/, and every non-English locale.
    grep -E '^(coll|brkitr|zone|curr|lang|region|unit|rbnf|translit)/' "$WASM_BLD/icu-items.txt" \
        > "$WASM_BLD/icu-remove.txt" || true
    grep -vE '/' "$WASM_BLD/icu-items.txt" | grep -E '\.res$' \
        | grep -vE '^(root|en|en_US|en_US_POSIX|pool|res_index|supplementalData|numberingSystems|metadata|likelySubtags|metaZones|timezoneTypes|windowsZones|keyTypeData|plurals|dayPeriods|grammaticalFeatures|genderList|icuver|icustd|tzdbNames|units|pluralRanges|zoneinfo64|characterProperties)\.res$' \
        >> "$WASM_BLD/icu-remove.txt" || true
    cp "$dat" "$out"
    "$icupkg" -r "$WASM_BLD/icu-remove.txt" "$out" > "$WASM_LOGS/icu-trim.log" 2>&1
    echo "icu data: $( du -h "$dat" | cut -f1 ) -> $( du -h "$out" | cut -f1 ) ($out)"

    # keep the host tools that are handy later (icupkg for re-trimming)
    mkdir -p "$WASM_HOST_PREFIX/bin"
    cp "$icupkg" "$WASM_HOST_PREFIX/bin/"

    ls "$WASM_PREFIX/lib/libicuuc.a" "$WASM_PREFIX/lib/libicui18n.a" >/dev/null
    cleanup icu icu-host icu-wasm
}

# ---------------------------------------------------------------------------
# Xerces-C (note 4)
# ---------------------------------------------------------------------------
stage_xerces() {
    fetch "https://archive.apache.org/dist/xerces/c/3/sources/xerces-c-$XERCES_VERSION.tar.xz"
    local src; src=$( unpack "xerces-c-$XERCES_VERSION.tar.xz" "xerces-c-$XERCES_VERSION" )
    cmake_dep xerces "$src" \
        -Dnetwork=OFF -Dthreads=OFF -Dtranscoder=icu -Dmessage-loader=inmemory \
        -Dxmlch-type=char16_t -DICU_ROOT="$WASM_PREFIX" \
        -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON
    ls "$WASM_PREFIX/lib/libxerces-c.a" >/dev/null
    cleanup "xerces-c-$XERCES_VERSION" xerces-build
}

# ---------------------------------------------------------------------------
# Boost (the compiled components FreeCAD's SetupBoost asks for, + headers)
# ---------------------------------------------------------------------------
stage_boost() {
    fetch "https://archives.boost.io/release/$BOOST_VERSION/source/boost_$BOOST_U.tar.bz2"
    local src; src=$( unpack "boost_$BOOST_U.tar.bz2" "boost_$BOOST_U" )
    # b2 itself is a native program: bootstrap with the host compiler.
    ( cd "$src" && run boost-bootstrap env -u CFLAGS -u CXXFLAGS -u LDFLAGS ./bootstrap.sh )
    # threading=multi is needed for Boost.Thread to be built at all; with no
    # -pthread on the command line emscripten gives it its single-threaded
    # pthread stubs, which is exactly what FreeCAD's uses (mutexes) need.
    # shellcheck disable=SC2086
    ( cd "$src" && run boost-build ./b2 -j"$JOBS" -q toolset=emscripten \
          link=static runtime-link=static threading=multi variant=release \
          --with-program_options --with-regex --with-thread --with-date_time \
          --with-filesystem --with-system --with-atomic --with-chrono \
          cxxflags="$WASM_CXXFLAGS -std=c++17" cflags="$WASM_CFLAGS" \
          linkflags="$WASM_LDFLAGS" \
          --prefix="$WASM_PREFIX" --layout=system install )
    ls "$WASM_PREFIX/lib/libboost_program_options.a" "$WASM_PREFIX/lib/libboost_thread.a" >/dev/null
    cleanup "boost_$BOOST_U"
}

# ---------------------------------------------------------------------------
# CPython (note 5)
# ---------------------------------------------------------------------------
stage_python() {
    fetch "https://www.python.org/ftp/python/$PYTHON_VERSION/Python-$PYTHON_VERSION.tar.xz"
    local src; src=$( unpack "Python-$PYTHON_VERSION.tar.xz" "Python-$PYTHON_VERSION" )
    local bld="$WASM_BLD/python-build"
    rm -rf "$bld"; mkdir -p "$bld"

    # Modules we cannot / do not want to build (their libraries would be
    # emscripten ports, see note 2, or they are meaningless in a browser).
    cat > "$bld/config.site-freecad" <<EOF
. "$src/Tools/wasm/config.site-wasm32-emscripten"
py_cv_module__bz2=n/a
py_cv_module__lzma=n/a
py_cv_module__sqlite3=n/a
py_cv_module__ctypes=n/a
py_cv_module__ctypes_test=n/a
py_cv_module__tkinter=n/a
py_cv_module__curses=n/a
py_cv_module__curses_panel=n/a
py_cv_module_readline=n/a
py_cv_module__dbm=n/a
py_cv_module__gdbm=n/a
py_cv_module__ssl=n/a
py_cv_module__hashlib=n/a
py_cv_module__uuid=n/a
py_cv_module_nis=n/a
py_cv_module__testcapi=n/a
py_cv_module__testinternalcapi=n/a
py_cv_module__testbuffer=n/a
py_cv_module__testimportmultiple=n/a
py_cv_module__testmultiphase=n/a
py_cv_module_xxsubtype=n/a
py_cv_module__xxtestfuzz=n/a
py_cv_module_xxlimited=n/a
py_cv_module_xxlimited_35=n/a
EOF

    ( cd "$bld" &&
      run python-configure env CONFIG_SITE="$bld/config.site-freecad" \
          CFLAGS="$WASM_CFLAGS" LDFLAGS="$WASM_LDFLAGS" \
          ZLIB_CFLAGS="-I$WASM_PREFIX/include" ZLIB_LIBS="-L$WASM_PREFIX/lib -lz" \
          emconfigure "$src/configure" -C \
          --host=wasm32-unknown-emscripten --build="$( "$src/config.guess" )" \
          --with-emscripten-target=node \
          --with-build-python="$WASM_HOST_PYTHON" \
          --disable-wasm-dynamic-linking --disable-wasm-pthreads \
          --disable-shared --without-pymalloc --disable-ipv6 \
          --disable-test-modules \
          --prefix="$WASM_PREFIX" &&
      grep -q '^MODULE_BUILDTYPE=static' Makefile &&
      run python-build   emmake make -j"$JOBS" &&
      run python-install emmake make install )

    ls "$WASM_PREFIX/lib/libpython$PY_MM.a" "$WASM_PREFIX/include/python$PY_MM/Python.h" >/dev/null

    # The stdlib for MEMFS: a zip, found by default on sys.path as
    # <prefix>/lib/python311.zip once PYTHONHOME is set (the module mounts it
    # at /freecad/lib/python311.zip).  Bytecode is compiled by the host
    # python - same minor version, so the .pyc magic matches.
    "$WASM_HOST_PYTHON" "$HERE/make-python-stdlib-zip.py" \
        "$WASM_PREFIX/lib/python$PY_MM" "$WASM_PREFIX/lib/python${PY_MM//./}.zip"

    cleanup "Python-$PYTHON_VERSION" python-build
}

# ---------------------------------------------------------------------------
# Qt 6 qtbase: Core + Xml, no GUI, no threads (note 6)
# ---------------------------------------------------------------------------
stage_qt() {
    fetch "https://download.qt.io/archive/qt/$QT_MM/$QT_VERSION/submodules/qtbase-everywhere-src-$QT_VERSION.tar.xz"
    local src; src=$( unpack "qtbase-everywhere-src-$QT_VERSION.tar.xz" "qtbase-everywhere-src-$QT_VERSION" )
    local host_qt
    host_qt=$( "$WASM_QT_HOST_PATH/lib/qt6/bin/qmake6" -query QT_VERSION 2>/dev/null ||
               qmake6 -query QT_VERSION )
    [ "$host_qt" = "$QT_VERSION" ] || die "host Qt is $host_qt, need $QT_VERSION (moc/rcc must match)"

    # Qt picks up the toolchain from -platform wasm-emscripten + EMSDK.
    ( mkdir -p "$WASM_BLD/qt-build" && cd "$WASM_BLD/qt-build" &&
      run qt-configure "$src/configure" -platform wasm-emscripten \
          -qt-host-path "$WASM_QT_HOST_PATH" \
          -prefix "$WASM_PREFIX" -static -release \
          -no-gui -no-widgets -no-dbus -no-opengl -no-feature-network -no-feature-sql \
          -no-feature-testlib -no-feature-printsupport \
          -no-icu -no-glib -no-pch -qt-zlib -qt-pcre -qt-doubleconversion \
          -nomake examples -nomake tests -nomake benchmarks \
          -- -DQT_HOST_PATH_CMAKE_DIR="$WASM_QT_HOST_CMAKE" \
             -DCMAKE_C_FLAGS="$WASM_CFLAGS" -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
             -DCMAKE_CXX_SCAN_FOR_MODULES=OFF &&
      run qt-build   cmake --build . -j"$JOBS" &&
      run qt-install cmake --install . )
    ls "$WASM_PREFIX/lib/libQt6Core.a" "$WASM_PREFIX/lib/cmake/Qt6Core/Qt6CoreConfig.cmake" >/dev/null
    cleanup "qtbase-everywhere-src-$QT_VERSION" qt-build
}

# ---------------------------------------------------------------------------
# OpenCASCADE (note 7)
# ---------------------------------------------------------------------------
stage_occt() {
    fetch_git https://github.com/Open-Cascade-SAS/OCCT "V${OCCT_VERSION//./_}" "occt-$OCCT_VERSION"
    cmake_dep occt "$WASM_SRC/occt-$OCCT_VERSION" \
        -DBUILD_LIBRARY_TYPE=Static \
        -DINSTALL_DIR="$WASM_PREFIX" \
        -DBUILD_MODULE_FoundationClasses=ON \
        -DBUILD_MODULE_ModelingData=ON \
        -DBUILD_MODULE_ModelingAlgorithms=ON \
        -DBUILD_MODULE_Visualization=OFF \
        -DBUILD_MODULE_ApplicationFramework=OFF \
        -DBUILD_MODULE_DataExchange=OFF \
        -DBUILD_MODULE_Draw=OFF \
        -DBUILD_ADDITIONAL_TOOLKITS="TKXSBase TKSTEPBase TKSTEPAttr TKSTEP209 TKSTEP TKIGES TKSTL" \
        -DBUILD_DOC_Overview=OFF -DBUILD_SAMPLES_QT=OFF -DBUILD_Inspector=OFF \
        -DUSE_FREETYPE=OFF -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_OPENGL=OFF -DUSE_GLES2=OFF \
        -DUSE_XLIB=OFF -DUSE_FREEIMAGE=OFF -DUSE_RAPIDJSON=OFF -DUSE_TBB=OFF \
        -DUSE_VTK=OFF -DUSE_DRACO=OFF -DUSE_FFMPEG=OFF -DUSE_OPENVR=OFF -DUSE_D3D=OFF \
        -DBUILD_RELEASE_DISABLE_EXCEPTIONS=OFF \
        -DCMAKE_CXX_STANDARD=17
    ls "$WASM_PREFIX/lib/libTKernel.a" "$WASM_PREFIX/lib/libTKSTEP.a" >/dev/null
    cleanup occt-build
}

# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------
ALL_STAGES=(zlib eigen fmt yamlcpp icu xerces boost python qt occt)
STAGES=( "${@:-}" )
[ -z "${STAGES[0]:-}" ] && STAGES=( "${ALL_STAGES[@]}" )

if [ "${STAGES[0]}" = list ]; then
    for s in "${ALL_STAGES[@]}"; do
        if is_done "$s"; then echo "done     $s ($( cat "$( stamp_file "$s" )" ))"; else echo "pending  $s"; fi
    done
    exit 0
fi

preflight
START=$( date +%s )

for stage in "${STAGES[@]}"; do
    case " ${ALL_STAGES[*]} " in
        *" $stage "*) ;;
        *) die "unknown stage '$stage' (have: ${ALL_STAGES[*]})" ;;
    esac
    if is_done "$stage"; then
        say "$stage: already built (rm $( stamp_file "$stage" ) to redo)"
        continue
    fi
    say "$stage"
    t0=$( date +%s )
    "stage_$stage"
    mark_done "$stage"
    echo "$stage: $(( $( date +%s ) - t0 ))s; disk $( df -h "$WASM_ROOT" | awk 'NR==2 {print $4 " free"}' )"
done

END=$( date +%s )
say "done in $(( (END - START) / 60 ))m $(( (END - START) % 60 ))s"
echo "prefix: $WASM_PREFIX ($( du -sh "$WASM_PREFIX" | cut -f1 ))"
