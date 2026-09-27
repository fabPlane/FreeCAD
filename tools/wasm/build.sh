#!/usr/bin/env bash
#
# Build the FreeCAD wasm targets (after tools/wasm/configure.sh).
#
#   tools/wasm/build.sh                     # freecad_wasm_smoke (+ all it needs)
#   tools/wasm/build.sh freecad_api         # the API module (src/Api C ABI)
#   tools/wasm/build.sh --smoke             # build, then run the smoke test in node
#   tools/wasm/build.sh --api               # build freecad_api, then run test-api.mjs
#   JOBS=2 tools/wasm/build.sh
#
# Output: $FREECAD_WASM_BUILD/wasm/<target>.{js,wasm,data}
#
# Never run two builds in one tree at the same time: that corrupts ninja's
# .ninja_deps, after which it rebuilds everything on every run (KiCad's
# host/STATUS-wasm-build.md, section 7).
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=env.sh
source "$HERE/env.sh"

JOBS="${JOBS:-3}"
RUN_SMOKE=0
RUN_API=0
TARGETS=()
for a in "$@"; do
    case "$a" in
        --smoke) RUN_SMOKE=1 ;;
        --api)   RUN_API=1; TARGETS+=( freecad_api ) ;;
        *) TARGETS+=( "$a" ) ;;
    esac
done
[ ${#TARGETS[@]} -gt 0 ] || TARGETS=( freecad_wasm_smoke )

[ -f "$FREECAD_WASM_BUILD/build.ninja" ] || { echo "not configured - run tools/wasm/configure.sh" >&2; exit 1; }

start=$( date +%s )
cmake --build "$FREECAD_WASM_BUILD" -j"$JOBS" --target "${TARGETS[@]}"
echo "build: $(( $( date +%s ) - start ))s"

OUT="$FREECAD_WASM_BUILD/wasm"
ls -la "$OUT"/*.js "$OUT"/*.wasm "$OUT"/*.data 2>/dev/null || true

if [ "$RUN_SMOKE" = 1 ]; then
    ( cd "$OUT" && node freecad_wasm_smoke.js )
fi
if [ "$RUN_API" = 1 ]; then
    node "$HERE/test-api.mjs" "$OUT"
fi
