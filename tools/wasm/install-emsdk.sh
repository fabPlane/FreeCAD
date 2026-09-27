#!/usr/bin/env bash
#
# Install and activate the pinned Emscripten SDK.
#
#   tools/wasm/install-emsdk.sh            # -> $EMSDK (default /home/user/emsdk)
#   EMSDK=/opt/emsdk tools/wasm/install-emsdk.sh
#
# The version is pinned in env.sh (WASM_EMSDK_VERSION, 6.0.9 - the same
# toolchain the KiCad fork's wasm module uses).  Everything in the dependency
# prefix is compiled with it; a different emcc may produce objects that link
# but disagree on exception-handling encoding or libc++ ABI, so rebuild the
# prefix (rm -rf $WASM_PREFIX $WASM_ROOT/.stamp-*) whenever this changes.
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
WASM_EMSDK_VERSION="${WASM_EMSDK_VERSION:-$( grep -o 'WASM_EMSDK_VERSION:-[0-9.]*' "$HERE/env.sh" | cut -d- -f2 )}"
EMSDK="${EMSDK:-/home/user/emsdk}"

if [ ! -d "$EMSDK/.git" ]; then
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK"
fi

cd "$EMSDK"
./emsdk install  "$WASM_EMSDK_VERSION"
./emsdk activate "$WASM_EMSDK_VERSION"

# shellcheck source=/dev/null
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
emcc --version | head -1
